#include "fuse/stage_c_state.h"

#include <cerrno>
#include <utility>

namespace eufs::fuse_adapter {

StageCState::StageCState(
    std::string image_path_value,
    std::unique_ptr<storage::MountedImageSession> session_value,
    std::unique_ptr<storage::ImageReader> reader_value,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer_value)
    : image_path(std::move(image_path_value)),
      session(std::move(session_value)),
      reader(std::move(reader_value)),
      mutation_observer(std::move(mutation_observer_value)) {}

bool StageCState::usable() const {
  return fatal_error_ == 0 && session != nullptr && reader != nullptr;
}

void StageCState::FailClosed(int error, std::string_view detail) {
  reader.reset();
  fatal_error_ = error < 0 ? error : -EIO;
  fatal_detail_.assign(detail);
}

int StageCState::ReloadReader(std::string* detail) {
  if (fatal_error_ != 0 || session == nullptr) {
    if (detail != nullptr) {
      detail->assign(fatal_detail_.empty() ? "Stage C state is unavailable"
                                           : fatal_detail_);
    }
    return -EIO;
  }

  int reader_fd = -1;
  int result = session->DuplicateFd(&reader_fd, detail);
  if (result != 0) {
    FailClosed(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }

  std::unique_ptr<storage::ImageReader> candidate;
  result = storage::ImageReader::AdoptLockedFd(reader_fd, &candidate, detail);
  if (result != 0) {
    FailClosed(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  reader = std::move(candidate);
  return 0;
}

int OpenStageCState(
    const std::string& image_path, std::unique_ptr<StageCState>* output,
    journal::RecoveryAction* recovery_action, std::string* detail,
    std::shared_ptr<journal::JournalControlIo> recovery_io,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer) {
  if (image_path.empty() || output == nullptr || recovery_action == nullptr) {
    if (detail != nullptr) {
      detail->assign(
          "image path, Stage C state output, and recovery action are required");
    }
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  std::unique_ptr<storage::MountedImageSession> session;
  int result =
      storage::MountedImageSession::Open(image_path, &session, detail);
  if (result != 0) {
    return result;
  }

  int store_fd = -1;
  result = session->DuplicateFd(&store_fd, detail);
  if (result != 0) {
    return result;
  }
  std::unique_ptr<journal::JournalControlStore> store;
  result = journal::JournalControlStore::AdoptLockedFd(
      store_fd, &store, detail, std::move(recovery_io));
  if (result != 0) {
    return result;
  }

  journal::RecoveryAction action{};
  result = store->ResolveRecovery(&action, detail);
  if (result != 0) {
    return result;
  }
  store.reset();

  int reader_fd = -1;
  result = session->DuplicateFd(&reader_fd, detail);
  if (result != 0) {
    return result;
  }
  std::unique_ptr<storage::ImageReader> reader;
  result = storage::ImageReader::AdoptLockedFd(reader_fd, &reader, detail);
  if (result != 0) {
    return result;
  }

  output->reset(new StageCState(image_path, std::move(session),
                               std::move(reader),
                               std::move(mutation_observer)));
  *recovery_action = action;
  return 0;
}

}  // namespace eufs::fuse_adapter
