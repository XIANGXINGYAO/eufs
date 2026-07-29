#pragma once

#include "journal/ondisk_journal.h"
#include "journal/ring_reservation.h"
#include "metadata/ondisk_format.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace eufs::journal {

class JournalControlIo {
 public:
  virtual ~JournalControlIo() = default;

  virtual ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                         off_t offset) = 0;
  virtual int Fdatasync(int fd) = 0;
};

struct DurableJournalBody {
  RingReservationPlan reservation;
  std::uint32_t entry_count{0};
  std::uint32_t descriptor_crc32c{0};
};

enum class RecoveryState {
  kEmpty,
  kUncommitted,
  kCommitted,
};

enum class RecoveryAction {
  kNoAction,
  kDiscarded,
  kReplayedAndCheckpointed,
};

enum class DurableStage {
  kOrderedData,
  kJournalBody,
  kControlExposure,
  kCommit,
  kHomeBlocks,
  kCheckpoint,
};

class DurableStageObserver {
 public:
  virtual ~DurableStageObserver() = default;
  virtual void OnDurableStage(DurableStage stage) = 0;
};

class ValidatedTransaction {
 public:
  ValidatedTransaction(const ValidatedTransaction&) = default;
  ValidatedTransaction(ValidatedTransaction&&) = default;
  ValidatedTransaction& operator=(const ValidatedTransaction&) = default;
  ValidatedTransaction& operator=(ValidatedTransaction&&) = default;

  std::uint64_t transaction_id() const { return transaction_id_; }
  std::uint32_t descriptor_ring_index() const {
    return descriptor_ring_index_;
  }
  std::uint32_t commit_ring_index() const { return commit_ring_index_; }
  const std::map<std::uint32_t, ondisk::Block>& metadata_after_images() const {
    return metadata_after_images_;
  }

 private:
  friend class JournalControlStore;

  ValidatedTransaction() = default;

  std::uint64_t transaction_id_{0};
  std::uint32_t descriptor_ring_index_{0};
  std::uint32_t commit_ring_index_{0};
  std::map<std::uint32_t, ondisk::Block> metadata_after_images_;
};

class JournalControlStore {
 public:
  ~JournalControlStore();

  JournalControlStore(const JournalControlStore&) = delete;
  JournalControlStore& operator=(const JournalControlStore&) = delete;

  static int Open(const std::string& image_path,
                  std::unique_ptr<JournalControlStore>* output,
                  std::string* detail,
                  std::shared_ptr<JournalControlIo> io = nullptr,
                  std::shared_ptr<DurableStageObserver> observer = nullptr);

  // Consumes locked_fd on every return path, including validation failures.
  static int AdoptLockedFd(
      int locked_fd, std::unique_ptr<JournalControlStore>* output,
      std::string* detail,
      std::shared_ptr<JournalControlIo> io = nullptr,
      std::shared_ptr<DurableStageObserver> observer = nullptr);

  const ondisk::Superblock& superblock() const { return superblock_; }
  const JournalControl& current() const { return current_; }
  ControlCopy current_copy() const { return current_copy_; }
  bool reload_required() const { return reload_required_; }
  const std::optional<DurableJournalBody>& durable_body() const {
    return durable_body_;
  }
  bool commit_durable() const { return commit_durable_; }
  const std::optional<ValidatedTransaction>& validated_transaction() const {
    return validated_transaction_;
  }

  int WriteOrderedDataAndUnexposedBody(
      std::uint32_t expected_total_blocks,
      const std::array<std::uint8_t, 16>& expected_filesystem_uuid,
      const std::map<std::uint32_t, ondisk::Block>& before_images,
      const std::map<std::uint32_t, ondisk::Block>&
          ordered_data_after_images,
      const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
      DurableJournalBody* output, std::string* detail);
  int WriteUnexposedBody(
      const RingReservationPlan& reservation,
      const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
      DurableJournalBody* output, std::string* detail);
  int ExposeDurableBody(std::string* detail);
  int WriteCommit(std::string* detail);
  int CompleteCommittedTransaction(std::string* detail);
  int ClassifyRecovery(RecoveryState* output, std::string* detail);
  int ResolveRecovery(RecoveryAction* output, std::string* detail);

 private:
  JournalControlStore(int fd, const ondisk::Superblock& superblock,
                      const JournalControl& current, ControlCopy current_copy,
                      std::shared_ptr<JournalControlIo> io,
                      std::shared_ptr<DurableStageObserver> observer);

  int PersistNext(const JournalControl& next, std::string* detail);

  int fd_;
  ondisk::Superblock superblock_;
  JournalControl current_;
  ControlCopy current_copy_;
  std::shared_ptr<JournalControlIo> io_;
  std::shared_ptr<DurableStageObserver> observer_;
  std::optional<DurableJournalBody> durable_body_;
  std::map<std::uint32_t, ondisk::Block> metadata_after_images_;
  std::optional<ValidatedTransaction> validated_transaction_;
  bool commit_durable_{false};
  bool checkpointed_{false};
  bool reload_required_{false};
};

}  // namespace eufs::journal
