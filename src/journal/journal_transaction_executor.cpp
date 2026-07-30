#include "journal/journal_transaction_executor.h"

#include <cerrno>
#include <memory>
#include <utility>

namespace eufs::journal {

int ExecuteJournalTransaction(
    storage::MountedImageSession& session,
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>&
        ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    std::shared_ptr<JournalControlIo> io,
    std::shared_ptr<DurableStageObserver> observer,
    bool* failure_requires_fail_closed, std::string* detail) {
  if (failure_requires_fail_closed == nullptr) {
    if (detail != nullptr) {
      detail->assign("journal execution requires a failure disposition output");
    }
    return -EINVAL;
  }
  *failure_requires_fail_closed = false;

  int mutation_fd = -1;
  int result = session.DuplicateFd(&mutation_fd, detail);
  if (result != 0) {
    return result;
  }

  *failure_requires_fail_closed = true;
  std::unique_ptr<JournalControlStore> store;
  result = JournalControlStore::AdoptLockedFd(
      mutation_fd, &store, detail, std::move(io), std::move(observer));
  if (result != 0) {
    return result;
  }

  DurableJournalBody body;
  result = store->WriteOrderedDataAndUnexposedBody(
      total_blocks, filesystem_uuid, before_images,
      ordered_data_after_images, metadata_after_images, &body, detail);
  if (result == 0) {
    result = store->ExposeDurableBody(detail);
  }
  if (result == 0) {
    result = store->WriteCommit(detail);
  }
  if (result == 0) {
    result = store->CompleteCommittedTransaction(detail);
  }
  if (result == 0) {
    *failure_requires_fail_closed = false;
  }
  return result;
}

}  // namespace eufs::journal
