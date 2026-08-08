// 验证 ordered data 先持久化，随后 descriptor 和 payload 写入尚未暴露的日志 ring。
// 在 control 更新前，恢复程序必须仍只看到旧事务边界。
#include "journal/journal_control_store.h"
#include "journal/ondisk_journal.h"
#include "journal/ring_reservation.h"
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"
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

std::string CreateImage(eufs::ondisk::Superblock* superblock) {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-body-store-test-XXXXXX");
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

std::map<std::uint32_t, eufs::ondisk::Block> MakeMetadataImages() {
  std::map<std::uint32_t, eufs::ondisk::Block> images;
  images[1].fill(0x31);
  images[3].fill(0x73);
  return images;
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

eufs::journal::RingReservationPlan MakeReservation(
    eufs::journal::JournalControlStore* store, std::size_t payload_count) {
  eufs::journal::RingReservationPlan reservation;
  std::string detail;
  Require(eufs::journal::PlanRingReservation(
              store->current(), payload_count, &reservation, &detail) == 0,
          detail.c_str());
  return reservation;
}

class FaultIo final : public eufs::journal::JournalControlIo {
 public:
  enum class Mode {
    kPass,
    kEintrThenShortWrites,
    kPartialWriteThenError,
    kSyncErrorAfterRealSync,
  };

  explicit FaultIo(Mode mode) : mode_(mode) {}

  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    ++pwrite_calls_;
    if (mode_ == Mode::kEintrThenShortWrites && !eintr_returned_) {
      eintr_returned_ = true;
      errno = EINTR;
      return -1;
    }
    if (mode_ == Mode::kPartialWriteThenError) {
      if (!partial_written_) {
        partial_written_ = true;
        return pwrite(fd, input, std::min<std::size_t>(64, size), offset);
      }
      errno = EIO;
      return -1;
    }
    const std::size_t write_size =
        mode_ == Mode::kEintrThenShortWrites
            ? std::min<std::size_t>(257, size)
            : size;
    return pwrite(fd, input, write_size, offset);
  }

  int Fdatasync(int fd) override {
    if (mode_ == Mode::kSyncErrorAfterRealSync) {
      const int result = fdatasync(fd);
      if (result != 0) {
        return result;
      }
      errno = EIO;
      return -1;
    }
    return fdatasync(fd);
  }

  std::size_t pwrite_calls() const { return pwrite_calls_; }

 private:
  Mode mode_;
  bool eintr_returned_{false};
  bool partial_written_{false};
  std::size_t pwrite_calls_{0};
};

void RequireSelectedControlClean(const std::string& path,
                                 std::uint64_t generation,
                                 std::uint32_t position) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  const auto& control = reader->journal_control();
  Require(control.generation == generation && control.head == position &&
              control.tail == position && control.used_blocks == 0,
          "selected control changed while body was unexposed");
}

void TestRealBodyBytesAndUnchangedControl() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  const auto control_a_before =
      ReadBlock(path, superblock.journal.start_block);
  const auto control_b_before =
      ReadBlock(path, superblock.journal.start_block + 1U);

  eufs::ondisk::Block commit_sentinel{};
  commit_sentinel.fill(0xC7);
  WriteBlock(path, RingPhysicalBlock(superblock, 3), commit_sentinel);

  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  const auto images = MakeMetadataImages();
  const auto reservation = MakeReservation(store.get(), images.size());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) == 0,
          detail.c_str());
  Require(store->current().used_blocks == 0 &&
              store->durable_body().has_value() && body.entry_count == 2 &&
              body.descriptor_crc32c != 0,
          "successful body write did not preserve the clean control state");
  eufs::journal::DurableJournalBody duplicate_output;
  Require(store->WriteUnexposedBody(reservation, images, &duplicate_output,
                                    &detail) == -EBUSY,
          "a second body replaced the durable unexposed transaction");

  const auto descriptor_bytes = ReadBlock(
      path, RingPhysicalBlock(superblock, reservation.descriptor_ring_index));
  eufs::journal::DescriptorRecord descriptor;
  Require(eufs::journal::DecodeDescriptor(descriptor_bytes, &descriptor,
                                          &detail),
          detail.c_str());
  Require(descriptor.transaction_id == reservation.transaction_id &&
              descriptor.filesystem_uuid == superblock.filesystem_uuid &&
              descriptor.entries.size() == 2 &&
              descriptor.checksum == body.descriptor_crc32c,
          "decoded descriptor identity or checksum is wrong");

  std::size_t index = 0;
  for (const auto& [home_block, payload] : images) {
    const auto& entry = descriptor.entries[index];
    const auto stored_payload = ReadBlock(
        path, RingPhysicalBlock(superblock,
                                reservation.payload_ring_indices[index]));
    Require(entry.home_block == home_block &&
                entry.payload_ring_index ==
                    reservation.payload_ring_indices[index] &&
                entry.payload_crc32c ==
                    eufs::ondisk::Crc32c(payload.data(), payload.size()) &&
                stored_payload == payload,
            "descriptor entry does not bind the stored payload");
    ++index;
  }
  Require(ReadBlock(path, RingPhysicalBlock(superblock,
                                            reservation.commit_ring_index)) ==
              commit_sentinel,
          "unexposed body writer modified the reserved COMMIT slot");
  Require(ReadBlock(path, superblock.journal.start_block) == control_a_before &&
              ReadBlock(path, superblock.journal.start_block + 1U) ==
                  control_b_before,
          "unexposed body writer modified A/B control blocks");
  store.reset();
  RequireSelectedControlClean(path, 0, 0);
  unlink(path.c_str());
}

void TestWrappedBodyPlacement() {
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
  const auto images = MakeMetadataImages();
  const auto reservation = MakeReservation(store.get(), images.size());
  Require(reservation.descriptor_ring_index == ring_blocks - 1U &&
              reservation.payload_ring_indices ==
                  std::vector<std::uint32_t>({0, 1}) &&
              reservation.commit_ring_index == 2,
          "wrapped body fixture did not cross the ring boundary");
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) == 0,
          detail.c_str());

  eufs::journal::DescriptorRecord descriptor;
  const auto descriptor_bytes = ReadBlock(
      path, RingPhysicalBlock(superblock, ring_blocks - 1U));
  Require(eufs::journal::DecodeDescriptor(descriptor_bytes, &descriptor,
                                          &detail) &&
              ReadBlock(path, RingPhysicalBlock(superblock, 0)) ==
                  images.at(1) &&
              ReadBlock(path, RingPhysicalBlock(superblock, 1)) ==
                  images.at(3),
          "wrapped descriptor or payload was written to the wrong block");
  store.reset();
  RequireSelectedControlClean(path, 9, ring_blocks - 1U);
  unlink(path.c_str());
}

void TestEintrAndShortWritesCompleteBody() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  auto io =
      std::make_shared<FaultIo>(FaultIo::Mode::kEintrThenShortWrites);
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail, io) ==
              0,
          detail.c_str());
  const auto images = MakeMetadataImages();
  const auto reservation = MakeReservation(store.get(), images.size());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) == 0 &&
              io->pwrite_calls() > 3 && !store->reload_required(),
          "EINTR/short writes did not complete the full journal body");
  store.reset();
  RequireSelectedControlClean(path, 0, 0);
  unlink(path.c_str());
}

void TestBodyWriteAndSyncFailuresStayUnexposed() {
  for (const auto mode : {FaultIo::Mode::kPartialWriteThenError,
                          FaultIo::Mode::kSyncErrorAfterRealSync}) {
    eufs::ondisk::Superblock superblock;
    const std::string path = CreateImage(&superblock);
    auto io = std::make_shared<FaultIo>(mode);
    std::unique_ptr<eufs::journal::JournalControlStore> store;
    std::string detail;
    Require(eufs::journal::JournalControlStore::Open(path, &store, &detail,
                                                     io) == 0,
            detail.c_str());
    const auto images = MakeMetadataImages();
    const auto reservation = MakeReservation(store.get(), images.size());
    eufs::journal::DurableJournalBody output;
    output.entry_count = 99;
    Require(store->WriteUnexposedBody(reservation, images, &output, &detail) ==
                -EIO &&
                store->reload_required() && output.entry_count == 99 &&
                !store->durable_body().has_value(),
            "body I/O failure changed authority or returned partial output");
    const std::size_t calls_after_failure = io->pwrite_calls();
    Require(store->ExposeDurableBody(&detail) == -EIO &&
                io->pwrite_calls() == calls_after_failure,
            "failed body Store attempted to expose control");
    store.reset();
    RequireSelectedControlClean(path, 0, 0);
    unlink(path.c_str());
  }
}

void TestValidationRejectsStaleAndForbiddenTargets() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  auto io = std::make_shared<FaultIo>(FaultIo::Mode::kPass);
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail, io) ==
              0,
          detail.c_str());

  auto images = MakeMetadataImages();
  auto stale = MakeReservation(store.get(), images.size());
  ++stale.transaction_id;
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(stale, images, &body, &detail) == -ESTALE &&
              io->pwrite_calls() == 0,
          "stale reservation reached journal I/O");

  images.clear();
  images[0].fill(0x44);
  auto reservation = MakeReservation(store.get(), images.size());
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) ==
              -EINVAL &&
              io->pwrite_calls() == 0,
          "superblock target reached journal I/O");

  images.clear();
  images[superblock.journal.start_block].fill(0x55);
  reservation = MakeReservation(store.get(), images.size());
  Require(store->WriteUnexposedBody(reservation, images, &body, &detail) ==
              -EINVAL &&
              io->pwrite_calls() == 0,
          "journal self-target reached journal I/O");
  store.reset();
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestRealBodyBytesAndUnchangedControl();
  TestWrappedBodyPlacement();
  TestEintrAndShortWritesCompleteBody();
  TestBodyWriteAndSyncFailuresStayUnexposed();
  TestValidationRejectsStaleAndForbiddenTargets();
  std::cout << "body=descriptor_payload wrap=ok commit=untouched control=clean "
               "short_write=ok eintr=ok io_error=latched\n";
  std::cout << "PASS: unexposed journal body store test\n";
  return 0;
}
