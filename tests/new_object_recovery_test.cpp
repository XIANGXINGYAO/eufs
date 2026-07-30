#include "checker/consistency_checker.h"
#include "journal/journal_control_store.h"
#include "metadata/new_object_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"

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

bool ReadRawBitmapBit(const std::string& path,
                      const eufs::ondisk::Region& region,
                      std::uint32_t bit) {
  const std::uint64_t byte_offset =
      static_cast<std::uint64_t>(region.start_block) *
          eufs::ondisk::kBlockSize +
      bit / 8U;
  std::uint8_t byte = 0;
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image for raw bitmap read");
  Require(PreadAll(fd, &byte, 1, byte_offset),
          "could not read raw bitmap byte");
  close(fd);
  return (byte & static_cast<std::uint8_t>(1U << (bit % 8U))) != 0;
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

void ApplyPlan(const std::string& path,
               const eufs::metadata::NewObjectPlan& plan) {
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
    output[index] = static_cast<char>(base + index % 17U);
  }
  return output;
}

struct Fixture {
  std::string path;
  eufs::ondisk::Superblock superblock;
  eufs::metadata::NewObjectPlan plan;
  std::string payload;
};

Fixture CreateFixture() {
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-object-recovery-XXXXXX");
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

  auto reader = OpenReader(fixture.path);
  eufs::metadata::NewObjectPlan first;
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "a.txt", "alpha", 0644, 1000, 1000, 10, &first,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  ApplyPlan(fixture.path, first);

  fixture.payload = Pattern(13U * eufs::ondisk::kBlockSize, 'A');
  reader = OpenReader(fixture.path);
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "b.bin", fixture.payload, 0644, 1000, 1000, 20,
              &fixture.plan, &detail) == 0 &&
              !fixture.plan.directory_grew &&
              fixture.plan.data_blocks.size() == 13 &&
              fixture.plan.file_indirect_block != 0,
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
            fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
            fixture.plan.before_images,
            fixture.plan.ordered_data_after_images,
            fixture.plan.metadata_after_images, &body, &detail) != 0 ||
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

void RequireOrderedDataDurable(const Fixture& fixture) {
  for (const std::uint32_t block : fixture.plan.data_blocks) {
    Require(ReadRawBlock(fixture.path, block) ==
                fixture.plan.ordered_data_after_images.at(block),
            "new object data was not durable before metadata publication");
  }
}

void RequireRawPublicationState(const Fixture& fixture, bool published) {
  const auto expected_directory =
      published
          ? fixture.plan.metadata_after_images.at(fixture.plan.directory_block)
          : fixture.plan.before_images.at(fixture.plan.directory_block);
  Require(ReadRawBlock(fixture.path, fixture.plan.directory_block) ==
              expected_directory,
          "raw directory block violates the COMMIT publication boundary");
  Require(ReadRawBitmapBit(fixture.path, fixture.superblock.inode_bitmap,
                           fixture.plan.inode_number - 1U) == published,
          "raw inode bitmap violates the COMMIT publication boundary");
  for (const std::uint32_t block : fixture.plan.data_blocks) {
    Require(ReadRawBitmapBit(fixture.path, fixture.superblock.block_bitmap,
                             block) == published,
            "raw data-block ownership violates the COMMIT boundary");
  }
  Require(ReadRawBitmapBit(fixture.path, fixture.superblock.block_bitmap,
                           fixture.plan.file_indirect_block) == published &&
              ReadRawBlock(fixture.path,
                           fixture.plan.file_indirect_block) ==
                  (published
                       ? fixture.plan.metadata_after_images.at(
                             fixture.plan.file_indirect_block)
                       : fixture.plan.before_images.at(
                             fixture.plan.file_indirect_block)),
          "file indirect block violates the COMMIT publication boundary");
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

void RequireNamespace(const Fixture& fixture, bool published) {
  auto reader = OpenReader(fixture.path);
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(reader->ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0,
          "pre-existing object disappeared during recovery");
  std::array<std::uint8_t, 5> alpha{};
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(inode_number, 0, alpha.data(), alpha.size(),
                           &bytes_read, &detail) == 0 &&
              bytes_read == alpha.size() &&
              std::memcmp(alpha.data(), "alpha", alpha.size()) == 0,
          "pre-existing object contents changed during recovery");

  const int lookup =
      reader->ResolvePath("/b.bin", &inode_number, &inode, &detail);
  if (!published) {
    Require(lookup == -ENOENT,
            "uncommitted object name became visible after recovery");
    return;
  }
  Require(lookup == 0 && inode_number == fixture.plan.inode_number &&
              inode.size == fixture.payload.size(),
          "committed object inode is missing or incomplete");
  std::vector<std::uint8_t> contents(fixture.payload.size());
  Require(reader->ReadFile(inode_number, 0, contents.data(), contents.size(),
                           &bytes_read, &detail) == 0 &&
              bytes_read == contents.size() &&
              std::memcmp(contents.data(), fixture.payload.data(),
                          contents.size()) == 0,
          "committed object payload is incomplete");
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
          "recovered new-object transaction is globally inconsistent");
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

  const bool home_is_published =
      stage == eufs::journal::DurableStage::kHomeBlocks ||
      stage == eufs::journal::DurableStage::kCheckpoint;
  RequireRawPublicationState(fixture, home_is_published);

  const auto action = Recover(fixture.path);
  const bool committed = stage == eufs::journal::DurableStage::kCommit ||
                         stage == eufs::journal::DurableStage::kHomeBlocks ||
                         stage == eufs::journal::DurableStage::kCheckpoint;
  if (stage == eufs::journal::DurableStage::kControlExposure) {
    Require(action == eufs::journal::RecoveryAction::kDiscarded,
            "uncommitted object transaction was not discarded");
  } else if (stage == eufs::journal::DurableStage::kCommit ||
             stage == eufs::journal::DurableStage::kHomeBlocks) {
    Require(action ==
                eufs::journal::RecoveryAction::kReplayedAndCheckpointed,
            "committed object transaction was not replayed");
  } else {
    Require(action == eufs::journal::RecoveryAction::kNoAction,
            "clean control unexpectedly requested object recovery");
  }

  RequireRawPublicationState(fixture, committed);
  RequireNamespace(fixture, committed);
  RequireHealthy(fixture.path);

  const auto after_first_recovery = ReadWholeImage(fixture.path);
  Require(Recover(fixture.path) == eufs::journal::RecoveryAction::kNoAction &&
              ReadWholeImage(fixture.path) == after_first_recovery,
          "second object recovery was not a byte-identical no-op");

  std::cout << "stage=" << StageName(stage)
            << " inode=" << fixture.plan.inode_number
            << " directory=" << fixture.plan.directory_block
            << " data0=" << fixture.plan.data_blocks.front()
            << " indirect=" << fixture.plan.file_indirect_block
            << " final=" << (committed ? "present" : "absent") << '\n';
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
  std::cout << "PASS: atomic new-object publication crash matrix\n";
  return 0;
}
