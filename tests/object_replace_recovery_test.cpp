// 在六个持久化阶段终止全量替换，证明恢复结果只能是完整旧对象或完整新对象。
#include "checker/consistency_checker.h"
#include "journal/journal_control_store.h"
#include "metadata/new_object_plan.h"
#include "metadata/object_replace_plan.h"
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
#include <memory>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

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

eufs::ondisk::Block ReadRawBlock(const std::string& path,
                                 std::uint32_t block_number) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image for raw block read");
  eufs::ondisk::Block output{};
  Require(PreadAll(fd, output.data(), output.size(),
                   static_cast<std::uint64_t>(block_number) *
                       eufs::ondisk::kBlockSize),
          "could not read raw image block");
  close(fd);
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
  Require(fd >= 0, "could not open image for raw bitmap read");
  Require(PreadAll(fd, &byte, 1, byte_offset),
          "could not read raw bitmap byte");
  close(fd);
  return (byte & static_cast<std::uint8_t>(1U << (block_number % 8U))) != 0;
}

std::vector<std::uint8_t> ReadWholeImage(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image snapshot");
  struct stat attributes {};
  Require(fstat(fd, &attributes) == 0 && attributes.st_size > 0,
          "could not stat image snapshot");
  std::vector<std::uint8_t> output(
      static_cast<std::size_t>(attributes.st_size));
  Require(PreadAll(fd, output.data(), output.size(), 0),
          "could not read image snapshot");
  close(fd);
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

template <typename Plan>
void ApplyPlan(const std::string& path, const Plan& plan) {
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail) == 0,
          detail.c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteOrderedDataAndUnexposedBody(
              plan.total_blocks, plan.filesystem_uuid, plan.before_images,
              plan.ordered_data_after_images, plan.metadata_after_images,
              &body, &detail) == 0,
          detail.c_str());
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  Require(store->WriteCommit(&detail) == 0, detail.c_str());
  Require(store->CompleteCommittedTransaction(&detail) == 0,
          detail.c_str());
}

std::string Pattern(std::size_t size, char base) {
  std::string output(size, '\0');
  for (std::size_t index = 0; index < output.size(); ++index) {
    output[index] = static_cast<char>(base + index % 19U);
  }
  return output;
}

struct Fixture {
  std::string path;
  eufs::ondisk::Superblock superblock;
  std::uint32_t inode_number{0};
  std::string old_payload;
  std::string new_payload;
  eufs::metadata::ObjectReplacePlan replace;
};

Fixture CreateFixture() {
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-replace-recovery-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  Fixture fixture;
  fixture.path = path_template.data();
  eufs::storage::MkfsOptions options;
  options.image_path = fixture.path;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 512;
  options.journal_blocks = 16;
  std::string detail;
  Require(eufs::storage::FormatImage(options, &fixture.superblock, &detail),
          detail.c_str());

  fixture.old_payload = Pattern(13U * eufs::ondisk::kBlockSize, 'A');
  auto reader = OpenReader(fixture.path);
  eufs::metadata::NewObjectPlan create;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "a", fixture.old_payload, 0644, 1000, 1000, 10,
              &create, &detail) == 0 &&
              create.file_indirect_block != 0,
          detail.c_str());
  fixture.inode_number = create.inode_number;
  reader.reset();
  ApplyPlan(fixture.path, create);

  fixture.new_payload = Pattern(5000, 'n');
  reader = OpenReader(fixture.path);
  Require(eufs::metadata::PrepareObjectReplace(
              *reader, fixture.inode_number, 1, fixture.new_payload, 20,
              &fixture.replace, &detail) == 0 &&
              fixture.replace.old_data_blocks.size() == 13 &&
              fixture.replace.old_indirect_block != 0 &&
              fixture.replace.new_data_blocks.size() == 2 &&
              fixture.replace.new_indirect_block == 0,
          detail.c_str());
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
    std::unique_ptr<eufs::journal::JournalControlStore> store;
    std::string detail;
    auto observer = std::make_shared<ExitAtStage>(stage);
    if (eufs::journal::JournalControlStore::Open(
            fixture.path, &store, &detail, nullptr, observer) != 0) {
      _exit(210);
    }
    eufs::journal::DurableJournalBody body;
    if (store->WriteOrderedDataAndUnexposedBody(
            fixture.replace.total_blocks, fixture.replace.filesystem_uuid,
            fixture.replace.before_images,
            fixture.replace.ordered_data_after_images,
            fixture.replace.metadata_after_images, &body, &detail) != 0 ||
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
          "child did not exit at requested replacement stage");
}

void RequireOrderedDataDurable(const Fixture& fixture) {
  for (const std::uint32_t block : fixture.replace.new_data_blocks) {
    Require(ReadRawBlock(fixture.path, block) ==
                fixture.replace.ordered_data_after_images.at(block),
            "new replacement data was not durable before publication");
  }
}

void RequireRawState(const Fixture& fixture, bool committed) {
  const auto inode = ReadRawInode(fixture.path, fixture.superblock,
                                  fixture.inode_number);
  Require(inode.size ==
                  (committed ? fixture.new_payload.size()
                             : fixture.old_payload.size()) &&
              inode.generation == (committed ? 2U : 1U) &&
              inode.indirect_block ==
                  (committed ? 0U : fixture.replace.old_indirect_block),
          "raw inode mixed old and new replacement metadata");
  for (const std::uint32_t block : fixture.replace.old_data_blocks) {
    Require(ReadRawBitmapBit(fixture.path, fixture.superblock, block) ==
                !committed,
            "old data ownership violates replacement COMMIT boundary");
  }
  Require(ReadRawBitmapBit(fixture.path, fixture.superblock,
                           fixture.replace.old_indirect_block) == !committed,
          "old indirect ownership violates replacement COMMIT boundary");
  for (const std::uint32_t block : fixture.replace.new_data_blocks) {
    Require(ReadRawBitmapBit(fixture.path, fixture.superblock, block) ==
                committed,
            "new data ownership violates replacement COMMIT boundary");
  }
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

void RequireObject(const Fixture& fixture, bool committed) {
  auto reader = OpenReader(fixture.path);
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(reader->ReadInode(fixture.inode_number, &inode, &detail) == 0,
          detail.c_str());
  const std::string& expected =
      committed ? fixture.new_payload : fixture.old_payload;
  std::string actual(static_cast<std::size_t>(inode.size), '\0');
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(
              fixture.inode_number, 0,
              reinterpret_cast<std::uint8_t*>(actual.data()), actual.size(),
              &bytes_read, &detail) == 0 &&
              bytes_read == actual.size() && actual == expected,
          "recovered replacement is not the complete expected generation");
}

void RequireHealthy(const std::string& path) {
  eufs::checker::ConsistencyReport report;
  std::string detail;
  Require(eufs::checker::CheckImage(path, &report, &detail) == 0,
          detail.c_str());
  Require(report.status == eufs::checker::ScanStatus::kComplete &&
              report.root_reachability_complete &&
              report.block_reference_scan_complete &&
              report.inode_reference_scan_complete && report.issues.empty(),
          "recovered replacement failed global consistency checks");
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
  RequireRawState(fixture, home_is_new);

  const auto action = Recover(fixture.path);
  const bool committed = stage == eufs::journal::DurableStage::kCommit ||
                         stage == eufs::journal::DurableStage::kHomeBlocks ||
                         stage == eufs::journal::DurableStage::kCheckpoint;
  if (stage == eufs::journal::DurableStage::kControlExposure) {
    Require(action == eufs::journal::RecoveryAction::kDiscarded,
            "uncommitted replacement was not discarded");
  } else if (stage == eufs::journal::DurableStage::kCommit ||
             stage == eufs::journal::DurableStage::kHomeBlocks) {
    Require(action ==
                eufs::journal::RecoveryAction::kReplayedAndCheckpointed,
            "committed replacement was not replayed");
  } else {
    Require(action == eufs::journal::RecoveryAction::kNoAction,
            "clean replacement boundary unexpectedly requested recovery");
  }

  RequireRawState(fixture, committed);
  RequireObject(fixture, committed);
  RequireHealthy(fixture.path);
  const auto after_first_recovery = ReadWholeImage(fixture.path);
  Require(Recover(fixture.path) == eufs::journal::RecoveryAction::kNoAction &&
              ReadWholeImage(fixture.path) == after_first_recovery,
          "second replacement recovery was not a byte-identical no-op");
  std::cout << "stage=" << StageName(stage)
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
  std::cout << "PASS: full-object replacement crash matrix\n";
  return 0;
}
