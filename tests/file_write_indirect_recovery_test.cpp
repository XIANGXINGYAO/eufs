// 让文件写入跨过 direct block 边界进入 single-indirect，再在事务阶段注入恢复场景。
// 验证数据块、间接索引块和 inode 元数据在崩溃后只能整体采用或整体丢弃。
#include "checker/consistency_checker.h"
#include "journal/journal_control_store.h"
#include "metadata/empty_file_create_plan.h"
#include "metadata/file_write_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "tests/support/writable_image.h"

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
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint32_t kFileInode = 2;
constexpr int kCrashExitCode = 200;

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
    const ssize_t result =
        pread(fd, output + completed, size - completed,
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

std::uint32_t GetLe32(const std::uint8_t* input) {
  return static_cast<std::uint32_t>(input[0]) |
         (static_cast<std::uint32_t>(input[1]) << 8U) |
         (static_cast<std::uint32_t>(input[2]) << 16U) |
         (static_cast<std::uint32_t>(input[3]) << 24U);
}

eufs::ondisk::Block ReadRawBlock(const std::string& path,
                                 std::uint32_t block_number) {
  eufs::ondisk::Block output{};
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image for raw block read");
  const bool read = PreadAll(
      fd, output.data(), output.size(),
      static_cast<std::uint64_t>(block_number) * eufs::ondisk::kBlockSize);
  close(fd);
  Require(read, "could not read raw image block");
  return output;
}

eufs::ondisk::InodeRecord ReadRawInode(
    const std::string& path, const eufs::ondisk::Superblock& superblock,
    std::uint32_t inode_number) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(inode_number - 1U) *
      eufs::ondisk::kInodeRecordSize;
  const std::uint32_t table_block =
      superblock.inode_table.start_block +
      static_cast<std::uint32_t>(byte_index / eufs::ondisk::kBlockSize);
  const std::size_t offset =
      static_cast<std::size_t>(byte_index % eufs::ondisk::kBlockSize);
  const auto block = ReadRawBlock(path, table_block);
  eufs::ondisk::InodeBytes bytes{};
  std::copy_n(block.begin() + offset, bytes.size(), bytes.begin());
  eufs::ondisk::InodeRecord output;
  std::string detail;
  Require(eufs::ondisk::DecodeInode(bytes, inode_number, &output, &detail),
          detail.c_str());
  return output;
}

bool ReadRawBitmapBit(const std::string& path,
                      const eufs::ondisk::Superblock& superblock,
                      std::uint32_t block_number) {
  const std::uint64_t byte_offset =
      static_cast<std::uint64_t>(superblock.block_bitmap.start_block) *
          eufs::ondisk::kBlockSize +
      block_number / 8U;
  std::uint8_t byte = 0;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image for bitmap read");
  const bool read = PreadAll(fd, &byte, 1, byte_offset);
  close(fd);
  Require(read, "could not read bitmap byte");
  return (byte & static_cast<std::uint8_t>(1U << (block_number % 8U))) != 0;
}

std::vector<std::uint8_t> ReadWholeImage(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image for snapshot");
  struct stat attributes {};
  Require(fstat(fd, &attributes) == 0 && attributes.st_size >= 0,
          "could not stat image for snapshot");
  std::vector<std::uint8_t> output(
      static_cast<std::size_t>(attributes.st_size));
  const bool read = PreadAll(fd, output.data(), output.size(), 0);
  close(fd);
  Require(read, "could not read image snapshot");
  return output;
}

std::unique_ptr<eufs::storage::ImageReader> OpenReader(
    const std::string& path) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  return reader;
}

eufs::ondisk::InodeRecord PlannedInode(
    const eufs::storage::ImageReader& reader,
    const eufs::metadata::FileWritePlan& plan) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(plan.inode_number - 1U) *
      eufs::ondisk::kInodeRecordSize;
  const std::uint32_t table_block =
      reader.superblock().inode_table.start_block +
      static_cast<std::uint32_t>(byte_index / eufs::ondisk::kBlockSize);
  const std::size_t offset =
      static_cast<std::size_t>(byte_index % eufs::ondisk::kBlockSize);
  const auto& after = plan.metadata_after_images.at(table_block);
  eufs::ondisk::InodeBytes bytes{};
  std::copy_n(after.begin() + offset, bytes.size(), bytes.begin());
  eufs::ondisk::InodeRecord output;
  std::string detail;
  Require(eufs::ondisk::DecodeInode(bytes, plan.inode_number, &output, &detail),
          detail.c_str());
  return output;
}

struct Fixture {
  std::string path;
  eufs::ondisk::Superblock superblock;
  eufs::metadata::FileWritePlan overwrite;
  eufs::ondisk::InodeRecord old_inode;
  eufs::ondisk::InodeRecord new_inode;
  std::uint32_t old_indirect{0};
  std::uint32_t new_indirect{0};
  std::uint32_t old_data{0};
  std::uint32_t new_data{0};
  std::vector<std::uint8_t> expected_old;
  std::vector<std::uint8_t> expected_new;
};

Fixture CreateFixture() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-indirect-recovery-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  Fixture fixture;
  fixture.path = path_template.data();
  eufs::storage::MkfsOptions options;
  options.image_path = fixture.path;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 1024;
  options.journal_blocks = 256;
  std::string detail;
  Require(eufs::storage::FormatImage(options, &fixture.superblock, &detail),
          detail.c_str());

  auto reader = OpenReader(fixture.path);
  eufs::metadata::EmptyFileCreatePlan create;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 10, &create, &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(fixture.path, create, &detail) == 0,
          detail.c_str());

  fixture.expected_old.assign(13U * eufs::ondisk::kBlockSize,
                              static_cast<std::uint8_t>('I'));
  reader = OpenReader(fixture.path);
  eufs::metadata::FileWritePlan initial;
  Require(eufs::metadata::PrepareFileWrite(
              *reader, kFileInode, 0,
              std::string_view(
                  reinterpret_cast<const char*>(fixture.expected_old.data()),
                  fixture.expected_old.size()),
              20, &initial, &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyFileWritePlan(fixture.path, initial, &detail) == 0,
          detail.c_str());

  reader = OpenReader(fixture.path);
  Require(reader->ReadInode(kFileInode, &fixture.old_inode, &detail) == 0,
          detail.c_str());
  fixture.old_indirect = fixture.old_inode.indirect_block;
  Require(fixture.old_indirect != 0,
          "13-block fixture did not allocate an indirect block");
  eufs::ondisk::Block old_indirect_bytes{};
  Require(reader->ReadBlock(fixture.old_indirect, &old_indirect_bytes, &detail) ==
              0,
          detail.c_str());
  fixture.old_data = GetLe32(old_indirect_bytes.data());

  Require(eufs::metadata::PrepareFileWrite(
              *reader, kFileInode,
              12ULL * eufs::ondisk::kBlockSize, "Q", 30,
              &fixture.overwrite, &detail) == 0,
          detail.c_str());
  fixture.new_inode = PlannedInode(*reader, fixture.overwrite);
  fixture.new_indirect = fixture.new_inode.indirect_block;
  Require(fixture.new_indirect != 0 &&
              fixture.new_indirect != fixture.old_indirect,
          "indirect overwrite did not reserve a new indirect block");
  const auto& new_indirect_bytes =
      fixture.overwrite.metadata_after_images.at(fixture.new_indirect);
  fixture.new_data = GetLe32(new_indirect_bytes.data());
  Require(fixture.new_data != 0 && fixture.new_data != fixture.old_data &&
              fixture.overwrite.ordered_data_after_images.count(
                  fixture.new_data) == 1,
          "indirect overwrite did not reserve a new data block");
  Require(fixture.overwrite.metadata_after_images.count(
              fixture.new_indirect) == 1 &&
              fixture.overwrite.ordered_data_after_images.count(
                  fixture.new_indirect) == 0,
          "new indirect block was not classified as journaled metadata");
  Require(fixture.new_inode.direct_blocks == fixture.old_inode.direct_blocks,
          "logical-block-12 overwrite changed direct mappings");
  reader.reset();

  fixture.expected_new = fixture.expected_old;
  fixture.expected_new[12U * eufs::ondisk::kBlockSize] =
      static_cast<std::uint8_t>('Q');
  return fixture;
}

class ExitAtStage final : public eufs::journal::DurableStageObserver {
 public:
  explicit ExitAtStage(eufs::journal::DurableStage target) : target_(target) {}

  void OnDurableStage(eufs::journal::DurableStage stage) override {
    if (stage == target_) {
      _exit(kCrashExitCode);
    }
  }

 private:
  eufs::journal::DurableStage target_;
};

void CrashTransaction(const Fixture& fixture,
                      eufs::journal::DurableStage stage) {
  const pid_t child = fork();
  Require(child >= 0, "fork failed");
  if (child == 0) {
    std::string detail;
    std::unique_ptr<eufs::journal::JournalControlStore> store;
    auto observer = std::make_shared<ExitAtStage>(stage);
    if (eufs::journal::JournalControlStore::Open(
            fixture.path, &store, &detail, nullptr, observer) != 0) {
      _exit(210);
    }
    eufs::journal::DurableJournalBody body;
    if (store->WriteOrderedDataAndUnexposedBody(
            fixture.overwrite.total_blocks, fixture.overwrite.filesystem_uuid,
            fixture.overwrite.before_images,
            fixture.overwrite.ordered_data_after_images,
            fixture.overwrite.metadata_after_images, &body, &detail) != 0 ||
        store->ExposeDurableBody(&detail) != 0 ||
        store->WriteCommit(&detail) != 0 ||
        store->CompleteCommittedTransaction(&detail) != 0) {
      _exit(211);
    }
    _exit(212);
  }

  int status = 0;
  Require(waitpid(child, &status, 0) == child, "waitpid failed");
  Require(WIFEXITED(status) && WEXITSTATUS(status) == kCrashExitCode,
          "child did not exit at the requested durable stage");
}

void RequireRawOldState(const Fixture& fixture) {
  const auto inode =
      ReadRawInode(fixture.path, fixture.superblock, kFileInode);
  Require(inode.indirect_block == fixture.old_indirect,
          "pre-COMMIT home inode stopped pointing to old indirect block");
  Require(GetLe32(ReadRawBlock(fixture.path, fixture.old_indirect).data()) ==
              fixture.old_data,
          "pre-COMMIT old indirect mapping changed");
  Require(ReadRawBitmapBit(fixture.path, fixture.superblock,
                           fixture.old_indirect) &&
              ReadRawBitmapBit(fixture.path, fixture.superblock,
                               fixture.old_data) &&
              !ReadRawBitmapBit(fixture.path, fixture.superblock,
                                fixture.new_indirect) &&
              !ReadRawBitmapBit(fixture.path, fixture.superblock,
                                fixture.new_data),
          "pre-COMMIT home bitmap did not preserve old ownership");
  Require(ReadRawBlock(fixture.path, fixture.new_indirect) ==
              fixture.overwrite.before_images.at(fixture.new_indirect),
          "pre-COMMIT indirect metadata escaped the journal into home");
}

void RequireRawNewState(const Fixture& fixture) {
  const auto inode =
      ReadRawInode(fixture.path, fixture.superblock, kFileInode);
  Require(inode.indirect_block == fixture.new_indirect,
          "committed home inode does not point to new indirect block");
  Require(GetLe32(ReadRawBlock(fixture.path, fixture.new_indirect).data()) ==
              fixture.new_data,
          "committed new indirect block does not point to new data");
  Require(!ReadRawBitmapBit(fixture.path, fixture.superblock,
                            fixture.old_indirect) &&
              !ReadRawBitmapBit(fixture.path, fixture.superblock,
                                fixture.old_data) &&
              ReadRawBitmapBit(fixture.path, fixture.superblock,
                               fixture.new_indirect) &&
              ReadRawBitmapBit(fixture.path, fixture.superblock,
                               fixture.new_data),
          "committed bitmap did not switch indirect/data ownership");
}

void RequireOrderedDataDurable(const Fixture& fixture) {
  Require(ReadRawBlock(fixture.path, fixture.new_data) ==
              fixture.overwrite.ordered_data_after_images.at(
                  fixture.new_data),
          "new indirect data block was not durable before metadata publish");
}

eufs::journal::RecoveryAction Recover(const std::string& path) {
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  eufs::journal::RecoveryAction action{};
  Require(store->ResolveRecovery(&action, &detail) == 0, detail.c_str());
  return action;
}

void RequireFileContents(const Fixture& fixture, bool committed) {
  auto reader = OpenReader(fixture.path);
  const auto& expected = committed ? fixture.expected_new : fixture.expected_old;
  std::vector<std::uint8_t> actual(expected.size());
  std::size_t bytes_read = 0;
  std::string detail;
  Require(reader->ReadFile(kFileInode, 0, actual.data(), actual.size(),
                           &bytes_read, &detail) == 0,
          detail.c_str());
  Require(bytes_read == expected.size() && actual == expected,
          "recovered file contents are neither the expected old nor new state");
}

void RequireHealthy(const std::string& path) {
  eufs::checker::ConsistencyReport report;
  std::string detail;
  Require(eufs::checker::CheckImage(path, &report, &detail) == 0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete &&
              report.bitmap_geometry_scan_complete &&
              report.root_inode_decoded &&
              report.root_reachability_complete &&
              report.block_reference_scan_complete &&
              report.inode_reference_scan_complete && report.issues.empty(),
          "recovered indirect transaction failed global consistency checks");
}

const char* StageName(eufs::journal::DurableStage stage) {
  switch (stage) {
    case eufs::journal::DurableStage::kOrderedData:
      return "ordered-data";
    case eufs::journal::DurableStage::kJournalBody:
      return "journal-body";
    case eufs::journal::DurableStage::kControlExposure:
      return "control-exposure";
    case eufs::journal::DurableStage::kCommit:
      return "commit";
    case eufs::journal::DurableStage::kHomeBlocks:
      return "home-blocks";
    case eufs::journal::DurableStage::kCheckpoint:
      return "checkpoint";
  }
  return "unknown";
}

void TestStage(eufs::journal::DurableStage stage) {
  Fixture fixture = CreateFixture();
  CrashTransaction(fixture, stage);
  RequireOrderedDataDurable(fixture);

  const bool home_is_new =
      stage == eufs::journal::DurableStage::kHomeBlocks ||
      stage == eufs::journal::DurableStage::kCheckpoint;
  if (home_is_new) {
    RequireRawNewState(fixture);
  } else {
    RequireRawOldState(fixture);
  }

  const eufs::journal::RecoveryAction action = Recover(fixture.path);
  const bool committed = stage == eufs::journal::DurableStage::kCommit ||
                         stage == eufs::journal::DurableStage::kHomeBlocks ||
                         stage == eufs::journal::DurableStage::kCheckpoint;
  if (stage == eufs::journal::DurableStage::kControlExposure) {
    Require(action == eufs::journal::RecoveryAction::kDiscarded,
            "exposed pre-COMMIT transaction was not discarded");
  } else if (stage == eufs::journal::DurableStage::kCommit ||
             stage == eufs::journal::DurableStage::kHomeBlocks) {
    Require(action ==
                eufs::journal::RecoveryAction::kReplayedAndCheckpointed,
            "committed indirect transaction was not replayed");
  } else {
    Require(action == eufs::journal::RecoveryAction::kNoAction,
            "clean control unexpectedly requested recovery");
  }

  if (committed) {
    RequireRawNewState(fixture);
  } else {
    RequireRawOldState(fixture);
  }
  RequireFileContents(fixture, committed);
  RequireHealthy(fixture.path);

  const auto after_first_recovery = ReadWholeImage(fixture.path);
  Require(Recover(fixture.path) == eufs::journal::RecoveryAction::kNoAction,
          "second recovery was not a no-op");
  Require(ReadWholeImage(fixture.path) == after_first_recovery,
          "second recovery changed the image bytes");

  std::cout << "stage=" << StageName(stage)
            << " old_indirect=" << fixture.old_indirect
            << " new_indirect=" << fixture.new_indirect
            << " old_data=" << fixture.old_data
            << " new_data=" << fixture.new_data
            << " final=" << (committed ? "new" : "old") << '\n';
  unlink(fixture.path.c_str());
}

}  // namespace

int main() {
  for (const auto stage : {
           eufs::journal::DurableStage::kOrderedData,
           eufs::journal::DurableStage::kJournalBody,
           eufs::journal::DurableStage::kControlExposure,
           eufs::journal::DurableStage::kCommit,
           eufs::journal::DurableStage::kHomeBlocks,
           eufs::journal::DurableStage::kCheckpoint}) {
    TestStage(stage);
  }
  std::cout << "PASS: single-indirect COW commit/recovery crash matrix\n";
  return 0;
}
