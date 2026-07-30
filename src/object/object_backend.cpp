#include "object/object_backend.h"

#include "journal/journal_transaction_executor.h"
#include "metadata/new_object_plan.h"

#include <cerrno>
#include <limits>
#include <map>
#include <sys/stat.h>
#include <utility>

namespace eufs::object_store {
namespace {

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

ObjectStat MakeObjectStat(std::uint32_t inode_number,
                          const ondisk::InodeRecord& inode) {
  return ObjectStat{inode_number, inode.size, inode.mtime_ns,
                    inode.generation};
}

}  // namespace

ObjectBackend::ObjectBackend(
    ObjectBackendOptions options,
    std::unique_ptr<storage::MountedImageSession> session,
    std::unique_ptr<storage::ImageReader> reader,
    std::shared_ptr<journal::JournalControlIo> mutation_io,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer)
    : options_(options),
      session_(std::move(session)),
      reader_(std::move(reader)),
      mutation_io_(std::move(mutation_io)),
      mutation_observer_(std::move(mutation_observer)) {}

int ObjectBackend::Open(
    const std::string& image_path, const ObjectBackendOptions& options,
    std::unique_ptr<ObjectBackend>* output,
    journal::RecoveryAction* recovery_action, std::string* detail,
    std::shared_ptr<journal::JournalControlIo> recovery_io,
    std::shared_ptr<journal::JournalControlIo> mutation_io,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer) {
  if (image_path.empty() || output == nullptr || recovery_action == nullptr ||
      (options.permissions & ~0777U) != 0) {
    SetDetail(detail,
              "image path, valid options, backend output, and recovery action "
              "are required");
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  std::unique_ptr<storage::MountedImageSession> session;
  int result = storage::MountedImageSession::Open(image_path, &session, detail);
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

  output->reset(new ObjectBackend(options, std::move(session),
                                  std::move(reader),
                                  std::move(mutation_io),
                                  std::move(mutation_observer)));
  *recovery_action = action;
  return 0;
}

int ObjectBackend::CheckUsableLocked(std::string* detail) const {
  if (fatal_error_ == 0 && session_ != nullptr && reader_ != nullptr) {
    return 0;
  }
  SetDetail(detail, fatal_detail_.empty() ? "object backend is unavailable"
                                          : fatal_detail_);
  return -EIO;
}

void ObjectBackend::FailClosedLocked(int error, std::string_view detail) {
  reader_.reset();
  fatal_error_ = error < 0 ? error : -EIO;
  fatal_detail_.assign(detail);
}

int ObjectBackend::ReloadReaderLocked(std::string* detail) {
  int reader_fd = -1;
  int result = session_->DuplicateFd(&reader_fd, detail);
  if (result != 0) {
    FailClosedLocked(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  std::unique_ptr<storage::ImageReader> candidate;
  result = storage::ImageReader::AdoptLockedFd(reader_fd, &candidate, detail);
  if (result != 0) {
    FailClosedLocked(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  reader_ = std::move(candidate);
  return 0;
}

int ObjectBackend::ResolveRegularLocked(std::string_view name,
                                        std::uint32_t* inode_number,
                                        ondisk::InodeRecord* inode,
                                        std::string* detail) const {
  if (!metadata::IsValidRootObjectName(name) || inode_number == nullptr ||
      inode == nullptr) {
    SetDetail(detail, "valid root object name and lookup outputs are required");
    return -EINVAL;
  }
  std::string path("/");
  path.append(name);
  const int result = reader_->ResolvePath(path, inode_number, inode, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISREG(inode->mode)) {
    SetDetail(detail, "object name does not resolve to a regular file");
    return -EISDIR;
  }
  return 0;
}

int ObjectBackend::ApplyLocked(
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>&
        ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    std::string* detail) {
  bool failure_requires_fail_closed = false;
  const int result = journal::ExecuteJournalTransaction(
      *session_, before_images, ordered_data_after_images,
      metadata_after_images, total_blocks, filesystem_uuid,
      mutation_io_, mutation_observer_, &failure_requires_fail_closed, detail);
  if (result != 0 && failure_requires_fail_closed) {
    FailClosedLocked(result, detail == nullptr ? std::string_view{} : *detail);
  }
  if (result != 0) {
    return result;
  }
  return ReloadReaderLocked(detail);
}

int ObjectBackend::PutIfAbsent(std::string_view name, std::string_view data,
                               std::uint64_t timestamp_ns,
                               std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    return result;
  }

  metadata::NewObjectPlan plan;
  result = metadata::PrepareNewRootObject(
      *reader_, name, data, options_.permissions, options_.uid, options_.gid,
      timestamp_ns, &plan, detail);
  if (result != 0) {
    return result;
  }
  return ApplyLocked(plan.before_images, plan.ordered_data_after_images,
                     plan.metadata_after_images, plan.total_blocks,
                     plan.filesystem_uuid, detail);
}

int ObjectBackend::Get(std::string_view name, std::string* output,
                       std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "object data output is required");
    return -EINVAL;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    return result;
  }

  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  result = ResolveRegularLocked(name, &inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  if (inode.size > std::numeric_limits<std::size_t>::max()) {
    SetDetail(detail, "object size cannot be represented in memory");
    return -EOVERFLOW;
  }

  std::string candidate(static_cast<std::size_t>(inode.size), '\0');
  std::size_t bytes_read = 0;
  if (!candidate.empty()) {
    result = reader_->ReadFile(
        inode_number, 0, reinterpret_cast<std::uint8_t*>(candidate.data()),
        candidate.size(), &bytes_read, detail);
    if (result != 0) {
      return result;
    }
  }
  if (bytes_read != candidate.size()) {
    SetDetail(detail, "object reader returned an incomplete payload");
    return -EUCLEAN;
  }
  *output = std::move(candidate);
  return 0;
}

int ObjectBackend::Stat(std::string_view name, ObjectStat* output,
                        std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "object stat output is required");
    return -EINVAL;
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    return result;
  }

  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  result = ResolveRegularLocked(name, &inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  *output = MakeObjectStat(inode_number, inode);
  return 0;
}

bool ObjectBackend::usable() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return fatal_error_ == 0 && session_ != nullptr && reader_ != nullptr;
}

}  // namespace eufs::object_store
