// 声明 FuseMountState 及其启动工厂函数。
#include "fuse/mount_state.h"

#include <cerrno>
#include <utility>

namespace eufs::fuse_adapter {

// 使用成员初始化列表接管调用者传入的资源，避免复制文件描述符所有权。
FuseMountState::FuseMountState(
    std::unique_ptr<storage::MountedImageSession> session_value,
    std::unique_ptr<storage::ImageReader> reader_value,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer_value)
    : session(std::move(session_value)),
      reader(std::move(reader_value)),
      mutation_observer(std::move(mutation_observer_value)) {}

bool FuseMountState::usable() const {
  // 三个条件必须同时满足：没有致命错误、镜像会话仍在、reader 仍在。
  return fatal_error_ == 0 && session != nullptr && reader != nullptr;
}

void FuseMountState::FailClosed(int error, std::string_view detail) {
  // 先销毁 reader，确保后续代码无法误用可能已经过期的解析视图。
  reader.reset();
  // 项目内部统一保存负 errno；调用者误传非负值时统一退化为 -EIO。
  fatal_error_ = error < 0 ? error : -EIO;
  // 保存最初故障的上下文，后续请求返回同一根因。
  fatal_detail_.assign(detail);
}

int FuseMountState::ReloadReader(std::string* detail) {
  if (fatal_error_ != 0 || session == nullptr) {
    if (detail != nullptr) {
      detail->assign(fatal_detail_.empty() ? "FUSE mount state is unavailable"
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

int OpenFuseMountState(
    const std::string& image_path, std::unique_ptr<FuseMountState>* output,
    journal::RecoveryAction* recovery_action, std::string* detail,
    std::shared_ptr<journal::JournalControlIo> recovery_io,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer) {
  if (image_path.empty() || output == nullptr || recovery_action == nullptr) {
    if (detail != nullptr) {
      detail->assign(
          "image path, FUSE mount state output, and recovery action are required");
    }
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  // 创建整个挂载期间唯一的镜像所有者；它的析构才会释放 flock。
  std::unique_ptr<storage::MountedImageSession> session;
  int result =
      storage::MountedImageSession::Open(image_path, &session, detail);
  if (result != 0) {
    return result;
  }

  // 恢复器使用复制的 fd，但独占锁仍由 session 持续持有。
  // 必须先恢复再创建 reader，因为 home metadata 可能尚未回放已提交事务。
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

  // 只有恢复得到稳定磁盘边界后，才允许创建并暴露 reader。
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

  // 所有步骤成功后才一次性发布完整状态，失败时 output 始终保持为空。
  output->reset(new FuseMountState(std::move(session),
                                   std::move(reader),
                                   std::move(mutation_observer)));
  *recovery_action = action;
  return 0;
}

}  // namespace eufs::fuse_adapter
