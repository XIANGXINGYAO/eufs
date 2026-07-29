#include "journal/journal_control_store.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace eufs::journal {
namespace {

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

int Corrupt(std::string* detail, std::string_view message) {
  SetDetail(detail, message);
  return -EUCLEAN;
}

int Corrupt(std::string* detail, std::string_view context,
            const std::string& cause) {
  if (detail != nullptr) {
    detail->assign(context);
    detail->append(": ");
    detail->append(cause);
  }
  return -EUCLEAN;
}

int PreadAll(int fd, std::uint8_t* output, std::size_t size,
             std::uint64_t offset, std::string_view operation,
             std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      if (detail != nullptr) {
        detail->assign(operation);
        detail->append(": ");
        detail->append(std::strerror(error_number));
      }
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

int PwriteAll(JournalControlIo* io, int fd, const std::uint8_t* input,
              std::size_t size, std::uint64_t offset,
              std::string_view operation, std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    errno = 0;
    const auto result = io->Pwrite(
        fd, input + completed, size - completed,
        static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 && errno != 0 ? errno : EIO;
      if (detail != nullptr) {
        detail->assign(operation);
        detail->append(": ");
        detail->append(std::strerror(error_number));
      }
      return -error_number;
    }
    if (static_cast<std::size_t>(result) > size - completed) {
      SetDetail(detail, "pwrite returned an invalid length");
      return -EIO;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

std::uint32_t Advance(std::uint32_t start, std::size_t distance,
                      std::uint32_t ring_blocks) {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(start) + distance) % ring_blocks);
}

bool BlockIsInRegion(std::uint32_t block, const ondisk::Region& region) {
  return block >= region.start_block &&
         static_cast<std::uint64_t>(block) <
             static_cast<std::uint64_t>(region.start_block) +
                 region.block_count;
}

bool IsValidMetadataTarget(const ondisk::Superblock& superblock,
                           std::uint32_t block) {
  return block != 0 && block < superblock.total_blocks &&
         !BlockIsInRegion(block, superblock.journal);
}

int ReadRingBlock(int fd, const ondisk::Superblock& superblock,
                  std::uint32_t ring_index, ondisk::Block* output,
                  std::string_view operation, std::string* detail) {
  const std::uint32_t ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  if (output == nullptr || ring_index >= ring_blocks) {
    return Corrupt(detail, "derived journal position is outside the ring");
  }
  const std::uint64_t physical_block =
      static_cast<std::uint64_t>(superblock.journal.start_block) +
      ondisk::kJournalControlBlockCount + ring_index;
  const std::uint64_t journal_end =
      static_cast<std::uint64_t>(superblock.journal.start_block) +
      superblock.journal.block_count;
  if (physical_block >= journal_end ||
      physical_block >= superblock.total_blocks) {
    return Corrupt(detail, "derived journal position maps outside the image");
  }
  return PreadAll(fd, output->data(), output->size(),
                  physical_block * ondisk::kBlockSize, operation, detail);
}

}  // namespace

int JournalControlStore::ClassifyRecovery(RecoveryState* output,
                                          std::string* detail) {
  if (output == nullptr) {
    SetDetail(detail, "recovery state output is required");
    return -EINVAL;
  }
  if (detail != nullptr) {
    detail->clear();
  }

  // Never leave an earlier validated transaction usable after a failed rescan.
  validated_transaction_.reset();
  if (reload_required_) {
    SetDetail(detail,
              "journal durability is uncertain; reopen and reselect before "
              "recovery classification");
    return -EIO;
  }
  if (current_.used_blocks == 0) {
    *output = RecoveryState::kEmpty;
    return 0;
  }
  if (current_.next_transaction_id <= 1) {
    return Corrupt(detail,
                   "nonempty journal control has no preceding transaction id");
  }

  ondisk::Block descriptor_bytes{};
  int result = ReadRingBlock(fd_, superblock_, current_.tail,
                             &descriptor_bytes, "pread journal descriptor",
                             detail);
  if (result != 0) {
    return result;
  }

  DescriptorRecord descriptor;
  std::string decode_error;
  if (!DecodeDescriptor(descriptor_bytes, &descriptor, &decode_error)) {
    return Corrupt(detail, "exposed descriptor is invalid", decode_error);
  }
  const std::uint64_t expected_transaction_id =
      current_.next_transaction_id - 1U;
  if (descriptor.filesystem_uuid != superblock_.filesystem_uuid) {
    return Corrupt(detail, "descriptor UUID does not match the image");
  }
  if (descriptor.transaction_id != expected_transaction_id) {
    return Corrupt(detail,
                   "descriptor transaction id does not match the control");
  }

  const std::size_t payload_count = descriptor.entries.size();
  if (descriptor.transaction_block_count != current_.used_blocks) {
    return Corrupt(detail,
                   "descriptor length does not match the exposed range");
  }
  for (std::size_t index = 0; index < payload_count; ++index) {
    const auto& entry = descriptor.entries[index];
    const std::uint32_t expected_position =
        Advance(current_.tail, index + 1U, current_.ring_blocks);
    if (entry.payload_ring_index != expected_position) {
      return Corrupt(detail,
                     "descriptor payload position violates the v1 grammar");
    }
    if (!IsValidMetadataTarget(superblock_, entry.home_block)) {
      return Corrupt(detail,
                     "descriptor targets a forbidden metadata home block");
    }
  }

  const std::uint32_t commit_ring_index =
      Advance(current_.tail, payload_count + 1U, current_.ring_blocks);
  const std::uint32_t derived_head =
      Advance(current_.tail, payload_count + 2U, current_.ring_blocks);
  if (derived_head != current_.head) {
    return Corrupt(detail,
                   "descriptor transaction end does not match control head");
  }

  ValidatedTransaction candidate;
  candidate.transaction_id_ = descriptor.transaction_id;
  candidate.descriptor_ring_index_ = current_.tail;
  candidate.commit_ring_index_ = commit_ring_index;
  for (const auto& entry : descriptor.entries) {
    ondisk::Block payload{};
    result = ReadRingBlock(fd_, superblock_, entry.payload_ring_index, &payload,
                           "pread journal payload", detail);
    if (result != 0) {
      return result;
    }
    if (ondisk::Crc32c(payload.data(), payload.size()) !=
        entry.payload_crc32c) {
      return Corrupt(detail, "exposed journal payload checksum mismatch");
    }
    candidate.metadata_after_images_.emplace(entry.home_block,
                                              std::move(payload));
  }

  ondisk::Block commit_bytes{};
  result = ReadRingBlock(fd_, superblock_, commit_ring_index, &commit_bytes,
                         "pread journal COMMIT", detail);
  if (result != 0) {
    return result;
  }

  CommitRecord commit;
  decode_error.clear();
  if (!DecodeCommit(commit_bytes, &commit, &decode_error) ||
      commit.transaction_id != descriptor.transaction_id) {
    *output = RecoveryState::kUncommitted;
    return 0;
  }
  if (!CommitMatchesDescriptor(descriptor, current_.tail, commit,
                               &decode_error)) {
    return Corrupt(detail, "current COMMIT is inconsistent", decode_error);
  }

  validated_transaction_ = std::move(candidate);
  *output = RecoveryState::kCommitted;
  return 0;
}

int JournalControlStore::ResolveRecovery(RecoveryAction* output,
                                         std::string* detail) {
  if (output == nullptr) {
    SetDetail(detail, "recovery action output is required");
    return -EINVAL;
  }
  if (detail != nullptr) {
    detail->clear();
  }
  if (reload_required_) {
    SetDetail(detail,
              "journal durability is uncertain; reopen and reselect before "
              "recovery execution");
    return -EIO;
  }
  if (durable_body_.has_value() || commit_durable_) {
    SetDetail(detail,
              "recovery execution requires a newly opened control store");
    return -EBUSY;
  }

  RecoveryState state{};
  int result = ClassifyRecovery(&state, detail);
  if (result != 0) {
    return result;
  }
  if (state == RecoveryState::kEmpty) {
    *output = RecoveryAction::kNoAction;
    return 0;
  }

  JournalControl clean = current_;
  clean.generation += std::uint64_t{1};
  clean.tail = clean.head;
  clean.used_blocks = 0;
  clean.checksum = 0;

  if (state == RecoveryState::kUncommitted) {
    result = PersistNext(clean, detail);
    if (result != 0) {
      return result;
    }
    validated_transaction_.reset();
    *output = RecoveryAction::kDiscarded;
    return 0;
  }

  if (!validated_transaction_.has_value()) {
    return Corrupt(detail,
                   "committed classification omitted its validated transaction");
  }
  for (const auto& [home_block, after_image] :
       validated_transaction_->metadata_after_images()) {
    if (!IsValidMetadataTarget(superblock_, home_block)) {
      return Corrupt(detail,
                     "validated transaction contains a forbidden home block");
    }
    result = PwriteAll(io_.get(), fd_, after_image.data(), after_image.size(),
                       static_cast<std::uint64_t>(home_block) *
                           ondisk::kBlockSize,
                       "pwrite recovery home block", detail);
    if (result != 0) {
      reload_required_ = true;
      return result;
    }
  }

  errno = 0;
  if (io_->Fdatasync(fd_) != 0) {
    const int error_number = errno != 0 ? errno : EIO;
    if (detail != nullptr) {
      detail->assign("fdatasync recovery home blocks: ");
      detail->append(std::strerror(error_number));
    }
    reload_required_ = true;
    return -error_number;
  }

  result = PersistNext(clean, detail);
  if (result != 0) {
    return result;
  }
  validated_transaction_.reset();
  *output = RecoveryAction::kReplayedAndCheckpointed;
  return 0;
}

}  // namespace eufs::journal
