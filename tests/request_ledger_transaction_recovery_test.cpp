// 证明对象发布与 Request-ID 结果记录共享一个 COMMIT；任何崩溃恢复后都不能分裂。
#include "checker/consistency_checker.h"
#include "journal/journal_transaction_executor.h"
#include "metadata/new_object_plan.h"
#include "object/request_ledger_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "storage/mounted_image_session.h"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
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

std::vector<std::uint8_t> ReadWholeImage(const std::string& path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(fd >= 0, "could not open image snapshot");
  const off_t size = lseek(fd, 0, SEEK_END);
  Require(size > 0, "could not determine image size");
  std::vector<std::uint8_t> output(static_cast<std::size_t>(size));
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

eufs::object_store::RequestLedgerRecord LedgerRecord(
    std::uint32_t committed_inode) {
  eufs::object_store::RequestLedgerRecord record;
  record.operation =
      eufs::object_store::MutationOperation::kCreateIfAbsent;
  record.result_kind = eufs::object_store::LedgerResultKind::kCommitted;
  for (std::size_t index = 0; index < record.request_id.size(); ++index) {
    record.request_id[index] = static_cast<std::uint8_t>(0x20U + index);
  }
  for (std::size_t index = 0; index < record.fingerprint.size(); ++index) {
    record.fingerprint[index] = static_cast<std::uint8_t>(0x40U + index);
  }
  record.result_code = eufs::object_store::LedgerResultCode::kOk;
  record.committed_inode = committed_inode;
  record.committed_generation = 1;
  return record;
}

struct Fixture {
  std::string path;
  std::string payload;
  eufs::metadata::NewObjectPlan object;
  eufs::object_store::RequestLedgerAppendPlan ledger;
};

Fixture CreateFixture() {
  std::array<char, 80> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-ledger-transaction-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  Fixture fixture;
  fixture.path = path_template.data();
  fixture.payload.assign(2U * eufs::ondisk::kBlockSize + 137U, 'L');
  eufs::storage::MkfsOptions options;
  options.image_path = fixture.path;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  options.request_ledger_entries = 64;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());

  auto reader = OpenReader(fixture.path);
  Require(eufs::metadata::PrepareNewRootObject(
              *reader, "atomic.bin", fixture.payload, 0644, 1000, 1000, 10,
              &fixture.object, &detail) == 0,
          detail.c_str());
  eufs::object_store::RequestLedgerIndex index;
  Require(eufs::object_store::ScanRequestLedger(*reader, &index, &detail) == 0,
          detail.c_str());
  Require(eufs::object_store::PrepareRequestLedgerAppend(
              *reader, index, LedgerRecord(fixture.object.inode_number),
              &fixture.ledger, &detail) == 0,
          detail.c_str());
  Require(eufs::object_store::MergeRequestLedgerAppend(
              fixture.ledger, fixture.object.total_blocks,
              fixture.object.filesystem_uuid, &fixture.object.before_images,
              &fixture.object.ordered_data_after_images,
              &fixture.object.metadata_after_images, &detail) == 0,
          detail.c_str());
  Require(fixture.object.ordered_data_after_images.count(
              fixture.ledger.physical_block) == 0 &&
              fixture.object.metadata_after_images.count(
                  fixture.ledger.physical_block) == 1,
          "ledger block was not classified as transactional metadata");
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

void CrashSingleTransaction(const Fixture& fixture,
                            eufs::journal::DurableStage stage) {
  const pid_t child = fork();
  Require(child >= 0, "fork failed");
  if (child == 0) {
    std::unique_ptr<eufs::storage::MountedImageSession> session;
    std::string detail;
    if (eufs::storage::MountedImageSession::Open(fixture.path, &session,
                                                  &detail) != 0) {
      _exit(210);
    }
    bool fail_closed = false;
    auto observer = std::make_shared<ExitAtStage>(stage);
    const int result = eufs::journal::ExecuteJournalTransaction(
        *session, fixture.object.before_images,
        fixture.object.ordered_data_after_images,
        fixture.object.metadata_after_images, fixture.object.total_blocks,
        fixture.object.filesystem_uuid, nullptr, observer, &fail_closed,
        &detail);
    _exit(result == 0 ? 211 : 212);
  }

  int status = 0;
  Require(waitpid(child, &status, 0) == child, "waitpid failed");
  Require(WIFEXITED(status) && WEXITSTATUS(status) == kCrashExitCode,
          "child did not exit at the requested durable stage");
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

void RequireCoupledState(const Fixture& fixture, bool committed) {
  auto reader = OpenReader(fixture.path);
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  const int lookup =
      reader->ResolvePath("/atomic.bin", &inode_number, &inode, &detail);
  const bool object_present = lookup == 0;
  if (!object_present) {
    Require(lookup == -ENOENT, "object lookup failed for an unexpected reason");
  }

  eufs::object_store::RequestLedgerIndex index;
  Require(eufs::object_store::ScanRequestLedger(*reader, &index, &detail) == 0,
          detail.c_str());
  const auto* record = index.Find(fixture.ledger.record.request_id);
  const bool ledger_present = record != nullptr;

  Require(object_present == ledger_present,
          "object and request ledger split across the COMMIT boundary");
  Require(object_present == committed,
          "recovered coupled state disagrees with durable COMMIT");
  if (!committed) {
    Require(index.size() == 0 && index.next_sequence() == 1,
            "uncommitted ledger record survived recovery");
    return;
  }

  Require(inode_number == fixture.object.inode_number &&
              inode.generation == 1 && inode.size == fixture.payload.size() &&
              index.size() == 1 && record->sequence == 1 &&
              record->committed_inode == inode_number &&
              record->committed_generation == inode.generation,
          "committed ledger result does not identify the published object");
  std::vector<std::uint8_t> contents(fixture.payload.size());
  std::size_t bytes_read = 0;
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
              report.issues.empty(),
          "recovered object-ledger transaction is inconsistent");
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
  CrashSingleTransaction(fixture, stage);

  const bool home_published =
      stage == eufs::journal::DurableStage::kHomeBlocks ||
      stage == eufs::journal::DurableStage::kCheckpoint;
  Require(ReadRawBlock(fixture.path, fixture.ledger.physical_block) ==
              (home_published ? fixture.ledger.after_image
                              : fixture.ledger.before_image),
          "raw ledger home block crossed the expected replay boundary");

  const auto action = Recover(fixture.path);
  const bool committed = stage == eufs::journal::DurableStage::kCommit ||
                         stage == eufs::journal::DurableStage::kHomeBlocks ||
                         stage == eufs::journal::DurableStage::kCheckpoint;
  if (stage == eufs::journal::DurableStage::kControlExposure) {
    Require(action == eufs::journal::RecoveryAction::kDiscarded,
            "uncommitted composite transaction was not discarded");
  } else if (stage == eufs::journal::DurableStage::kCommit ||
             stage == eufs::journal::DurableStage::kHomeBlocks) {
    Require(action ==
                eufs::journal::RecoveryAction::kReplayedAndCheckpointed,
            "committed composite transaction was not replayed");
  } else {
    Require(action == eufs::journal::RecoveryAction::kNoAction,
            "clean control unexpectedly requested composite recovery");
  }

  RequireCoupledState(fixture, committed);
  RequireHealthy(fixture.path);
  const auto stable = ReadWholeImage(fixture.path);
  Require(Recover(fixture.path) == eufs::journal::RecoveryAction::kNoAction &&
              ReadWholeImage(fixture.path) == stable,
          "second composite recovery was not a byte-identical no-op");

  std::cout << "stage=" << StageName(stage)
            << " object_inode=" << fixture.object.inode_number
            << " ledger_block=" << fixture.ledger.physical_block
            << " final=" << (committed ? "both" : "neither") << '\n';
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
  std::cout << "PASS: object and request ledger share one recovery decision\n";
  return 0;
}
