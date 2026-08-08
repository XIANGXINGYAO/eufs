// 验证 JournalControlStore 接管已锁 fd 后的 control 读取、切换和持久化顺序。
// 该测试保护“只写另一份副本、同步成功后才承认新代际”的核心规则。
#include "journal/journal_control_store.h"
#include "journal/ondisk_journal.h"
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
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
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
  std::strcpy(path_template.data(), "/tmp/eufs-control-store-test-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, superblock, &detail),
          detail.c_str());
  return options.image_path;
}

std::uint64_t ControlOffset(const eufs::ondisk::Superblock& superblock,
                            eufs::journal::ControlCopy copy) {
  const std::uint32_t relative =
      copy == eufs::journal::ControlCopy::kA ? 0U : 1U;
  return static_cast<std::uint64_t>(superblock.journal.start_block + relative) *
         eufs::ondisk::kBlockSize;
}

std::map<std::uint32_t, eufs::ondisk::Block> MakeMetadataImages() {
  std::map<std::uint32_t, eufs::ondisk::Block> images;
  images[1].fill(0x11);
  images[2].fill(0x22);
  return images;
}

void RequireReaderState(const std::string& path,
                        eufs::journal::ControlCopy expected_copy,
                        std::uint64_t generation, std::uint32_t head,
                        std::uint32_t tail, std::uint32_t used_blocks) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  const auto& control = reader->journal_control();
  Require(reader->journal_control_copy() == expected_copy &&
              control.generation == generation && control.head == head &&
              control.tail == tail && control.used_blocks == used_blocks,
          "reopened reader selected an unexpected journal control state");
}

void RewriteWrapFixture(const std::string& path,
                        const eufs::ondisk::Superblock& superblock) {
  eufs::journal::JournalControl control_a;
  control_a.ring_blocks =
      superblock.journal.block_count - eufs::ondisk::kJournalControlBlockCount;
  control_a.filesystem_uuid = superblock.filesystem_uuid;
  control_a.generation = std::numeric_limits<std::uint64_t>::max();
  control_a.next_transaction_id = 1;
  auto control_b = control_a;
  control_b.generation = control_a.generation - 1U;

  eufs::ondisk::Block bytes_a{};
  eufs::ondisk::Block bytes_b{};
  std::string detail;
  Require(eufs::journal::EncodeControl(control_a, &bytes_a, nullptr, &detail) &&
              eufs::journal::EncodeControl(control_b, &bytes_b, nullptr,
                                            &detail),
          detail.c_str());
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "wrap fixture open failed");
  Require(PwriteAll(fd, bytes_a.data(), bytes_a.size(),
                    ControlOffset(superblock,
                                  eufs::journal::ControlCopy::kA)) &&
              PwriteAll(fd, bytes_b.data(), bytes_b.size(),
                        ControlOffset(superblock,
                                      eufs::journal::ControlCopy::kB)),
          "wrap fixture control write failed");
  Require(fdatasync(fd) == 0, "wrap fixture fdatasync failed");
  close(fd);
}

class FaultIo final : public eufs::journal::JournalControlIo {
 public:
  enum class Mode {
    kWriteErrorBeforeMutation,
    kPartialWriteThenError,
    kSyncErrorAfterRealSync,
  };

  void Arm(Mode mode) {
    mode_ = mode;
    partial_written_ = false;
  }

  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    ++pwrite_calls_;
    if (!mode_.has_value()) {
      return pwrite(fd, input, size, offset);
    }
    if (*mode_ == Mode::kWriteErrorBeforeMutation) {
      errno = EIO;
      return -1;
    }
    if (*mode_ == Mode::kPartialWriteThenError) {
      if (!partial_written_) {
        partial_written_ = true;
        return pwrite(fd, input, std::min<std::size_t>(64, size), offset);
      }
      errno = EIO;
      return -1;
    }
    return pwrite(fd, input, size, offset);
  }

  int Fdatasync(int fd) override {
    if (mode_.has_value() && *mode_ == Mode::kSyncErrorAfterRealSync) {
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
  std::optional<Mode> mode_;
  bool partial_written_{false};
  std::size_t pwrite_calls_{0};
};

eufs::journal::RingReservationPlan WriteBody(
    eufs::journal::JournalControlStore* store, std::string* detail) {
  const auto images = MakeMetadataImages();
  eufs::journal::RingReservationPlan reservation;
  Require(eufs::journal::PlanRingReservation(
              store->current(), images.size(), &reservation, detail) == 0,
          detail->c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteUnexposedBody(reservation, images, &body, detail) == 0,
          detail->c_str());
  Require(body.entry_count == images.size() &&
              body.descriptor_crc32c != 0,
          "durable journal body result is incomplete");
  return reservation;
}

void TestNormalAlternation() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  Require(store->current_copy() == eufs::journal::ControlCopy::kA,
          "initial equal controls did not deterministically select A");
  Require(store->ExposeDurableBody(&detail) == -EPERM,
          "control exposure succeeded without a durable journal body");

  const auto reservation = WriteBody(store.get(), &detail);
  Require(store->current().used_blocks == 0 &&
              store->current_copy() == eufs::journal::ControlCopy::kA,
          "writing an unexposed body changed the selected control");
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  Require(store->current_copy() == eufs::journal::ControlCopy::kB &&
              store->current().generation == 1 &&
              store->current().head == 4 && store->current().used_blocks == 4,
          "A to B control update did not publish the reserved state");
  Require(store->current().head == reservation.exposed_control.head,
          "exposed control does not match the durable body reservation");
  store.reset();
  RequireReaderState(path, eufs::journal::ControlCopy::kB, 1, 4, 0, 4);
  unlink(path.c_str());
}

void TestGenerationWrap() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  RewriteWrapFixture(path, superblock);

  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  Require(store->current_copy() == eufs::journal::ControlCopy::kA &&
              store->current().generation ==
                  std::numeric_limits<std::uint64_t>::max(),
          "wrap fixture did not select generation UINT64_MAX");
  const auto wrapped = WriteBody(store.get(), &detail);
  Require(wrapped.exposed_control.generation == 0,
          "unsigned generation successor did not wrap to zero");
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  store.reset();
  RequireReaderState(path, eufs::journal::ControlCopy::kB, 0, 4, 0, 4);
  unlink(path.c_str());
}

void TestWriteFailureBeforeMutation() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  auto io = std::make_shared<FaultIo>();
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail, io) ==
              0,
          detail.c_str());
  WriteBody(store.get(), &detail);
  io->Arm(FaultIo::Mode::kWriteErrorBeforeMutation);
  Require(store->ExposeDurableBody(&detail) == -EIO &&
              store->reload_required() &&
              store->current_copy() == eufs::journal::ControlCopy::kA,
          "write failure did not latch reload-required state");
  const std::size_t calls_after_failure = io->pwrite_calls();
  Require(store->ExposeDurableBody(&detail) == -EIO &&
              io->pwrite_calls() == calls_after_failure,
          "reload-required store attempted another control write");
  store.reset();
  RequireReaderState(path, eufs::journal::ControlCopy::kA, 0, 0, 0, 0);
  unlink(path.c_str());
}

void TestPartialWriteFailure() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  auto io = std::make_shared<FaultIo>();
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail, io) ==
              0,
          detail.c_str());
  WriteBody(store.get(), &detail);
  io->Arm(FaultIo::Mode::kPartialWriteThenError);
  Require(store->ExposeDurableBody(&detail) == -EIO &&
              store->reload_required(),
          "partial control write did not require reload");
  store.reset();
  RequireReaderState(path, eufs::journal::ControlCopy::kA, 0, 0, 0, 0);
  unlink(path.c_str());
}

void TestSyncFailureIsAmbiguous() {
  eufs::ondisk::Superblock superblock;
  const std::string path = CreateImage(&superblock);
  auto io = std::make_shared<FaultIo>();
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail, io) ==
              0,
          detail.c_str());
  WriteBody(store.get(), &detail);
  io->Arm(FaultIo::Mode::kSyncErrorAfterRealSync);
  Require(store->ExposeDurableBody(&detail) == -EIO &&
              store->reload_required() &&
              store->current_copy() == eufs::journal::ControlCopy::kA &&
              store->current().generation == 0,
          "sync failure incorrectly advanced in-memory authority");
  store.reset();

  RequireReaderState(path, eufs::journal::ControlCopy::kB, 1, 4, 0, 4);
  unlink(path.c_str());
}

}  // namespace

int main() {
  TestNormalAlternation();
  TestGenerationWrap();
  TestWriteFailureBeforeMutation();
  TestPartialWriteFailure();
  TestSyncFailureIsAmbiguous();
  std::cout << "exposure=body_before_control wrap=max_to_zero "
               "write_error=reload partial=B_rejected sync_error=B_possible\n";
  std::cout << "PASS: persistent inactive journal-control update test\n";
  return 0;
}
