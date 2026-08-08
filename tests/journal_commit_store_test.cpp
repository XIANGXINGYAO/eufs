// 验证 COMMIT 只能写在 descriptor 推导的唯一 ring 位置，并覆盖完整事务校验信息。
// 合法 COMMIT 是恢复采用事务的必要条件，但不是脱离 descriptor 的独立真相。
#include "journal/journal_control_store.h"
#include "journal/ondisk_journal.h"
#include "journal/ring_reservation.h"
#include "metadata/ondisk_format.h"
#include "storage/mkfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool PreadAll(int fd, std::uint8_t* output, std::size_t size,
              std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

bool PwriteAll(int fd, const std::uint8_t* input, std::size_t size,
               std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pwrite(fd, input + completed, size - completed,
                               static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

std::uint64_t BlockOffset(std::uint32_t block) {
  return static_cast<std::uint64_t>(block) * eufs::ondisk::kBlockSize;
}

std::uint32_t RingPhysicalBlock(
    const eufs::ondisk::Superblock& superblock, std::uint32_t ring_index) {
  return superblock.journal.start_block +
         eufs::ondisk::kJournalControlBlockCount + ring_index;
}

eufs::ondisk::Block ReadBlock(const std::string& path,
                              std::uint32_t block_number) {
  eufs::ondisk::Block block{};
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "open image for block read failed");
  Require(PreadAll(fd, block.data(), block.size(), BlockOffset(block_number)),
          "pread image block failed");
  close(fd);
  return block;
}

void WriteBlock(const std::string& path, std::uint32_t block_number,
                const eufs::ondisk::Block& block) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "open image for block write failed");
  Require(PwriteAll(fd, block.data(), block.size(), BlockOffset(block_number)),
          "pwrite image block failed");
  Require(fdatasync(fd) == 0, "fdatasync image block failed");
  close(fd);
}

std::string CreateImage(eufs::ondisk::Superblock* superblock) {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-commit-store-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 8;
  std::string detail;
  Require(eufs::storage::FormatImage(options, superblock, &detail),
          detail.c_str());
  return options.image_path;
}

std::map<std::uint32_t, eufs::ondisk::Block> MakeMetadataImages() {
  std::map<std::uint32_t, eufs::ondisk::Block> images;
  images[1].fill(0x31);
  images[3].fill(0x73);
  return images;
}

eufs::journal::DurableJournalBody WriteBodyAndExpose(
    eufs::journal::JournalControlStore* store,
    const std::map<std::uint32_t, eufs::ondisk::Block>& images,
    std::string* detail) {
  eufs::journal::RingReservationPlan reservation;
  Require(eufs::journal::PlanRingReservation(
              store->current(), images.size(), &reservation, detail) == 0,
          detail->c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, detail) == 0,
          detail->c_str());
  Require(store->ExposeDurableBody(detail) == 0, detail->c_str());
  return body;
}

class FaultIo final : public eufs::journal::JournalControlIo {
 public:
  enum class ArmedFailure { kNone, kPartialWriteThenError, kSyncError };

  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    ++pwrite_calls_;
    if (fail_next_pwrite_) {
      errno = EIO;
      return -1;
    }
    if (armed_failure_ == ArmedFailure::kPartialWriteThenError) {
      armed_failure_ = ArmedFailure::kNone;
      fail_next_pwrite_ = true;
      return pwrite(fd, input, std::min<std::size_t>(64, size), offset);
    }
    return pwrite(fd, input, size, offset);
  }

  int Fdatasync(int fd) override {
    ++sync_calls_;
    const int result = fdatasync(fd);
    if (result != 0) {
      return result;
    }
    if (armed_failure_ == ArmedFailure::kSyncError) {
      armed_failure_ = ArmedFailure::kNone;
      errno = EIO;
      return -1;
    }
    return 0;
  }

  void Arm(ArmedFailure failure) { armed_failure_ = failure; }
  std::size_t pwrite_calls() const { return pwrite_calls_; }
  std::size_t sync_calls() const { return sync_calls_; }

 private:
  ArmedFailure armed_failure_{ArmedFailure::kNone};
  bool fail_next_pwrite_{false};
  std::size_t pwrite_calls_{0};
  std::size_t sync_calls_{0};
};

void WriteStaleCommit(const std::string& path,
                      const eufs::ondisk::Superblock& superblock,
                      std::uint32_t commit_ring_index) {
  eufs::journal::CommitRecord stale;
  stale.transaction_id = 99;
  stale.filesystem_uuid = superblock.filesystem_uuid;
  stale.entry_count = 2;
  stale.transaction_block_count = 4;
  stale.descriptor_ring_index = 0;
  stale.descriptor_crc32c = 0x12345678U;
  eufs::ondisk::Block encoded{};
  std::string detail;
  Require(eufs::journal::EncodeCommit(stale, &encoded, &detail),
          detail.c_str());
  WriteBlock(path, RingPhysicalBlock(superblock, commit_ring_index), encoded);
}

void RewriteCleanControl(const std::string& path,
                         const eufs::ondisk::Superblock& superblock,
                         std::uint32_t position) {
  eufs::journal::JournalControl control;
  control.ring_blocks =
      superblock.journal.block_count -
      eufs::ondisk::kJournalControlBlockCount;
  control.filesystem_uuid = superblock.filesystem_uuid;
  control.generation = 9;
  control.head = position;
  control.tail = position;
  control.next_transaction_id = 17;
  eufs::ondisk::Block encoded{};
  std::string detail;
  Require(eufs::journal::EncodeControl(control, &encoded, nullptr, &detail),
          detail.c_str());
  WriteBlock(path, superblock.journal.start_block, encoded);
  WriteBlock(path, superblock.journal.start_block + 1U, encoded);
}

eufs::journal::DescriptorRecord ReadDescriptor(
    const std::string& path, const eufs::ondisk::Superblock& superblock,
    std::uint32_t ring_index) {
  eufs::journal::DescriptorRecord descriptor;
  std::string detail;
  Require(eufs::journal::DecodeDescriptor(
              ReadBlock(path, RingPhysicalBlock(superblock, ring_index)),
              &descriptor, &detail),
          detail.c_str());
  return descriptor;
}

void TestCommitRequiresExactExposedBodyAndBindsBytes() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  const auto images = MakeMetadataImages();
  const auto home_one_before = ReadBlock(path, 1);
  const auto home_three_before = ReadBlock(path, 3);
  WriteStaleCommit(path, superblock, 3);
  const auto stale_commit = ReadBlock(path, RingPhysicalBlock(superblock, 3));

  auto io = std::make_shared<FaultIo>();
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail, io) ==
              0,
          detail.c_str());
  Require(store->WriteCommit(&detail) == -EPERM && io->pwrite_calls() == 0,
          "COMMIT succeeded before a durable body existed");

  eufs::journal::RingReservationPlan reservation;
  Require(eufs::journal::PlanRingReservation(
              store->current(), images.size(), &reservation, &detail) == 0,
          detail.c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) == 0,
          detail.c_str());
  const std::size_t writes_after_body = io->pwrite_calls();
  Require(store->WriteCommit(&detail) == -EPERM &&
              io->pwrite_calls() == writes_after_body &&
              ReadBlock(path, RingPhysicalBlock(superblock, 3)) == stale_commit,
          "COMMIT succeeded before control exposure");

  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  const auto control_a_exposed =
      ReadBlock(path, superblock.journal.start_block);
  const auto control_b_exposed =
      ReadBlock(path, superblock.journal.start_block + 1U);
  Require(ReadBlock(path, RingPhysicalBlock(superblock, 3)) == stale_commit,
          "body/control publication modified the stale COMMIT slot");

  Require(store->WriteCommit(&detail) == 0 && store->commit_durable(),
          detail.c_str());
  eufs::journal::CommitRecord commit;
  Require(eufs::journal::DecodeCommit(
              ReadBlock(path, RingPhysicalBlock(superblock, 3)), &commit,
              &detail),
          detail.c_str());
  const auto descriptor = ReadDescriptor(
      path, superblock, body.reservation.descriptor_ring_index);
  Require(eufs::journal::CommitMatchesDescriptor(
              descriptor, body.reservation.descriptor_ring_index, commit,
              &detail),
          detail.c_str());
  Require(commit.transaction_id == body.reservation.transaction_id &&
              commit.filesystem_uuid == superblock.filesystem_uuid &&
              commit.entry_count == body.entry_count &&
              commit.transaction_block_count == body.entry_count + 2U &&
              commit.descriptor_ring_index ==
                  body.reservation.descriptor_ring_index &&
              commit.descriptor_crc32c == body.descriptor_crc32c &&
              commit.checksum != 0,
          "durable COMMIT does not bind the exact body");
  Require(ReadBlock(path, superblock.journal.start_block) ==
                  control_a_exposed &&
              ReadBlock(path, superblock.journal.start_block + 1U) ==
                  control_b_exposed &&
              ReadBlock(path, 1) == home_one_before &&
              ReadBlock(path, 3) == home_three_before,
          "COMMIT write modified control or home metadata");

  const std::size_t writes_after_commit = io->pwrite_calls();
  const std::size_t syncs_after_commit = io->sync_calls();
  Require(store->WriteCommit(&detail) == -EALREADY &&
              io->pwrite_calls() == writes_after_commit &&
              io->sync_calls() == syncs_after_commit,
          "a second COMMIT performed image I/O");
  store.reset();
  unlink(path.c_str());
}

void TestWrappedCommitUsesReservedPhysicalSlot() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  const std::uint32_t ring_blocks =
      superblock.journal.block_count -
      eufs::ondisk::kJournalControlBlockCount;
  RewriteCleanControl(path, superblock, ring_blocks - 1U);

  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  const auto body = WriteBodyAndExpose(store.get(), MakeMetadataImages(),
                                       &detail);
  Require(body.reservation.descriptor_ring_index == ring_blocks - 1U &&
              body.reservation.payload_ring_indices ==
                  std::vector<std::uint32_t>({0, 1}) &&
              body.reservation.commit_ring_index == 2,
          "wrapped COMMIT fixture did not cross the ring boundary");
  Require(store->WriteCommit(&detail) == 0, detail.c_str());

  eufs::journal::CommitRecord commit;
  Require(eufs::journal::DecodeCommit(
              ReadBlock(path, RingPhysicalBlock(superblock, 2)), &commit,
              &detail),
          detail.c_str());
  const auto descriptor =
      ReadDescriptor(path, superblock, ring_blocks - 1U);
  Require(commit.transaction_id == 17 &&
              eufs::journal::CommitMatchesDescriptor(
                  descriptor, ring_blocks - 1U, commit, &detail),
          "wrapped COMMIT does not bind the descriptor at the ring end");
  store.reset();
  unlink(path.c_str());
}

void TestCommitIoFailuresLatchAndStayOffHomeMetadata() {
  for (const auto failure :
       {FaultIo::ArmedFailure::kPartialWriteThenError,
        FaultIo::ArmedFailure::kSyncError}) {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto images = MakeMetadataImages();
    const auto home_one_before = ReadBlock(path, 1);
    const auto home_three_before = ReadBlock(path, 3);
    eufs::ondisk::Block sentinel{};
    sentinel.fill(0xC7);
    WriteBlock(path, RingPhysicalBlock(superblock, 3), sentinel);

    auto io = std::make_shared<FaultIo>();
    std::unique_ptr<eufs::journal::JournalControlStore> store;
    std::string detail;
    Require(eufs::journal::JournalControlStore::Open(path, &store, &detail,
                                                      io) == 0,
            detail.c_str());
    const auto body = WriteBodyAndExpose(store.get(), images, &detail);
    const auto control_a_exposed =
        ReadBlock(path, superblock.journal.start_block);
    const auto control_b_exposed =
        ReadBlock(path, superblock.journal.start_block + 1U);
    io->Arm(failure);
    Require(store->WriteCommit(&detail) == -EIO &&
                store->reload_required() && !store->commit_durable(),
            "COMMIT I/O failure advanced in-memory commit state");
    const std::size_t writes_after_failure = io->pwrite_calls();
    const std::size_t syncs_after_failure = io->sync_calls();
    Require(store->WriteCommit(&detail) == -EIO &&
                io->pwrite_calls() == writes_after_failure &&
                io->sync_calls() == syncs_after_failure,
            "failed COMMIT Store attempted another image mutation");
    Require(ReadBlock(path, superblock.journal.start_block) ==
                    control_a_exposed &&
                ReadBlock(path, superblock.journal.start_block + 1U) ==
                    control_b_exposed &&
                ReadBlock(path, 1) == home_one_before &&
                ReadBlock(path, 3) == home_three_before,
            "COMMIT failure modified control or home metadata");

    if (failure == FaultIo::ArmedFailure::kSyncError) {
      eufs::journal::CommitRecord commit;
      Require(eufs::journal::DecodeCommit(
                  ReadBlock(path, RingPhysicalBlock(superblock, 3)), &commit,
                  &detail),
              "sync-error fixture did not leave a valid possible COMMIT");
      const auto descriptor = ReadDescriptor(
          path, superblock, body.reservation.descriptor_ring_index);
      Require(eufs::journal::CommitMatchesDescriptor(
                  descriptor, body.reservation.descriptor_ring_index, commit,
                  &detail),
              "sync-error COMMIT does not match the durable body");
    }
    store.reset();
    unlink(path.c_str());
  }
}

void TestCommittedHomeFailuresRecoverAndCheckpoint() {
  for (const auto failure :
       {FaultIo::ArmedFailure::kPartialWriteThenError,
        FaultIo::ArmedFailure::kSyncError}) {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    const auto images = MakeMetadataImages();
    auto io = std::make_shared<FaultIo>();
    std::unique_ptr<eufs::journal::JournalControlStore> store;
    std::string detail;
    Require(eufs::journal::JournalControlStore::Open(path, &store, &detail,
                                                      io) == 0,
            detail.c_str());
    WriteBodyAndExpose(store.get(), images, &detail);
    Require(store->WriteCommit(&detail) == 0, detail.c_str());

    io->Arm(failure);
    Require(store->CompleteCommittedTransaction(&detail) == -EIO &&
                store->reload_required(),
            "committed home failure did not latch the Store");
    const std::size_t writes_after_failure = io->pwrite_calls();
    const std::size_t syncs_after_failure = io->sync_calls();
    Require(store->CompleteCommittedTransaction(&detail) == -EIO &&
                io->pwrite_calls() == writes_after_failure &&
                io->sync_calls() == syncs_after_failure,
            "latched committed-home Store attempted another mutation");
    store.reset();

    Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) ==
                0,
            detail.c_str());
    eufs::journal::RecoveryAction action{};
    Require(store->ResolveRecovery(&action, &detail) == 0,
            detail.c_str());
    Require(action ==
                    eufs::journal::RecoveryAction::kReplayedAndCheckpointed &&
                store->current().used_blocks == 0 &&
                store->current().head == store->current().tail,
            "reopen did not replay and checkpoint the durable COMMIT");
    for (const auto& [home_block, after_image] : images) {
      Require(ReadBlock(path, home_block) == after_image,
              "recovery did not restore a complete committed after-image");
    }
    store.reset();
    unlink(path.c_str());
  }
}

}  // namespace

int main() {
  TestCommitRequiresExactExposedBodyAndBindsBytes();
  TestWrappedCommitUsesReservedPhysicalSlot();
  TestCommitIoFailuresLatchAndStayOffHomeMetadata();
  TestCommittedHomeFailuresRecoverAndCheckpoint();
  std::cout << "commit=exact_body old_slot=overwritten precondition=exposed "
               "write_error=latched sync_error=disk_fact\n";
  std::cout << "PASS: durable journal COMMIT store test\n";
  return 0;
}
