#include "journal/journal_control_store.h"
#include "metadata/empty_file_create_plan.h"
#include "metadata/first_block_write_plan.h"
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "storage/writable_image.h"

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

struct FirstWriteFixture {
  std::string path;
  eufs::ondisk::Superblock superblock;
  eufs::metadata::FirstBlockWritePlan plan;
};

FirstWriteFixture CreateFirstWriteFixture() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-recovery-exec-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  FirstWriteFixture fixture;
  fixture.path = path_template.data();
  eufs::storage::MkfsOptions options;
  options.image_path = fixture.path;
  options.image_size_bytes = 64ULL * 1024ULL * 1024ULL;
  options.total_inodes = 1024;
  options.journal_blocks = 256;
  std::string detail;
  Require(eufs::storage::FormatImage(options, &fixture.superblock, &detail),
          detail.c_str());

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(fixture.path, &reader, &detail) == 0,
          detail.c_str());
  eufs::metadata::EmptyFileCreatePlan create_plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 100ULL, &create_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(fixture.path, create_plan, &detail) ==
              0,
          detail.c_str());

  Require(eufs::storage::ImageReader::Open(fixture.path, &reader, &detail) == 0,
          detail.c_str());
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, 2, "hello", 200ULL, &fixture.plan, &detail) == 0,
          detail.c_str());
  reader.reset();
  return fixture;
}

class FaultIo final : public eufs::journal::JournalControlIo {
 public:
  FaultIo(std::size_t direct_error_pwrite_call,
          std::size_t partial_pwrite_call,
          std::size_t sync_error_after_real_call)
      : direct_error_pwrite_call_(direct_error_pwrite_call),
        partial_pwrite_call_(partial_pwrite_call),
        sync_error_after_real_call_(sync_error_after_real_call) {}

  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    ++pwrite_calls_;
    if (fail_next_pwrite_ ||
        pwrite_calls_ == direct_error_pwrite_call_) {
      fail_next_pwrite_ = false;
      errno = EIO;
      return -1;
    }
    if (pwrite_calls_ == partial_pwrite_call_) {
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
    if (sync_calls_ == sync_error_after_real_call_) {
      errno = EIO;
      return -1;
    }
    return 0;
  }

  std::size_t pwrite_calls() const { return pwrite_calls_; }
  std::size_t sync_calls() const { return sync_calls_; }

 private:
  std::size_t direct_error_pwrite_call_{0};
  std::size_t partial_pwrite_call_{0};
  std::size_t sync_error_after_real_call_{0};
  std::size_t pwrite_calls_{0};
  std::size_t sync_calls_{0};
  bool fail_next_pwrite_{false};
};

std::unique_ptr<eufs::journal::JournalControlStore> OpenStore(
    const std::string& path, std::shared_ptr<eufs::journal::JournalControlIo> io =
                                 nullptr) {
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(path, &store, &detail,
                                                    std::move(io)) == 0,
          detail.c_str());
  return store;
}

eufs::journal::DurableJournalBody StageFirstWrite(
    const FirstWriteFixture& fixture, bool write_commit) {
  auto store = OpenStore(fixture.path);
  eufs::journal::DurableJournalBody body;
  std::string detail;
  Require(store->WriteOrderedDataAndUnexposedBody(
              fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
              fixture.plan.before_images,
              fixture.plan.ordered_data_after_images,
              fixture.plan.metadata_after_images, &body, &detail) == 0,
          detail.c_str());
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  if (write_commit) {
    Require(store->WriteCommit(&detail) == 0, detail.c_str());
  }
  return body;
}

std::vector<eufs::ondisk::Block> ReadTransactionRing(
    const FirstWriteFixture& fixture,
    const eufs::journal::DurableJournalBody& body) {
  std::vector<eufs::ondisk::Block> blocks;
  blocks.push_back(ReadBlock(
      fixture.path,
      RingPhysicalBlock(fixture.superblock,
                        body.reservation.descriptor_ring_index)));
  for (const auto ring_index : body.reservation.payload_ring_indices) {
    blocks.push_back(ReadBlock(
        fixture.path, RingPhysicalBlock(fixture.superblock, ring_index)));
  }
  blocks.push_back(ReadBlock(
      fixture.path,
      RingPhysicalBlock(fixture.superblock,
                        body.reservation.commit_ring_index)));
  return blocks;
}

void RequireCleanControl(const std::string& path, std::uint64_t generation,
                         std::uint32_t position,
                         std::uint64_t next_transaction_id) {
  auto store = OpenStore(path);
  Require(store->current().generation == generation &&
              store->current().head == position &&
              store->current().tail == position &&
              store->current().used_blocks == 0 &&
              store->current().next_transaction_id == next_transaction_id,
          "checkpoint did not publish the exact clean control successor");
}

void RequireEmptyFileSemantics(const FirstWriteFixture& fixture) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(fixture.path, &reader, &detail) == 0,
          detail.c_str());
  eufs::ondisk::InodeRecord inode;
  Require(reader->ReadInode(2, &inode, &detail) == 0 && inode.size == 0 &&
              inode.direct_blocks[0] == 0 &&
              !reader->IsBlockAllocated(fixture.plan.data_block),
          "discarded first write changed visible file metadata");
}

void RequireHelloSemantics(const FirstWriteFixture& fixture) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(fixture.path, &reader, &detail) == 0,
          detail.c_str());
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  Require(reader->ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0,
          detail.c_str());
  Require(inode_number == 2 && inode.size == 5 &&
              inode.direct_blocks[0] == fixture.plan.data_block &&
              reader->IsBlockAllocated(fixture.plan.data_block),
          "replayed metadata does not describe the planned file data");
  std::array<std::uint8_t, 8> content{};
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(inode_number, 0, content.data(), content.size(),
                           &bytes_read, &detail) == 0,
          detail.c_str());
  Require(bytes_read == 5 &&
              std::equal(content.begin(), content.begin() + bytes_read,
                         reinterpret_cast<const std::uint8_t*>("hello")),
          "replayed first-write transaction did not produce hello");
}

void FinishCommittedRecovery(const FirstWriteFixture& fixture) {
  auto store = OpenStore(fixture.path);
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == 0 &&
              action ==
                  eufs::journal::RecoveryAction::kReplayedAndCheckpointed,
          detail.c_str());
}

void TestEmptyAndActiveWriterGuards() {
  {
    auto fixture = CreateFirstWriteFixture();
    auto io = std::make_shared<FaultIo>(0, 0, 0);
    auto store = OpenStore(fixture.path, io);
    eufs::journal::RecoveryAction action =
        eufs::journal::RecoveryAction::kDiscarded;
    std::string detail;
    Require(store->ResolveRecovery(&action, &detail) == 0 &&
                action == eufs::journal::RecoveryAction::kNoAction &&
                io->pwrite_calls() == 0 && io->sync_calls() == 0,
            "empty recovery performed image I/O");
    store.reset();
    unlink(fixture.path.c_str());
  }

  {
    auto fixture = CreateFirstWriteFixture();
    auto io = std::make_shared<FaultIo>(0, 0, 0);
    auto store = OpenStore(fixture.path, io);
    eufs::journal::DurableJournalBody body;
    std::string detail;
    Require(store->WriteOrderedDataAndUnexposedBody(
                fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
                fixture.plan.before_images,
                fixture.plan.ordered_data_after_images,
                fixture.plan.metadata_after_images, &body, &detail) == 0,
            detail.c_str());
    Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
    const auto writes_before = io->pwrite_calls();
    const auto syncs_before = io->sync_calls();
    eufs::journal::RecoveryAction action =
        eufs::journal::RecoveryAction::kNoAction;
    Require(store->ResolveRecovery(&action, &detail) == -EBUSY &&
                io->pwrite_calls() == writes_before &&
                io->sync_calls() == syncs_before,
            "active writer Store entered mount-time recovery");
    store.reset();
    unlink(fixture.path.c_str());
  }
}

void TestUncommittedDiscardPublishesOnlyCleanControl() {
  auto fixture = CreateFirstWriteFixture();
  const auto body = StageFirstWrite(fixture, false);
  const auto ring_before = ReadTransactionRing(fixture, body);
  for (const auto& [block, after_image] :
       fixture.plan.metadata_after_images) {
    (void)after_image;
    Require(ReadBlock(fixture.path, block) ==
                fixture.plan.before_images.at(block),
            "uncommitted fixture changed home metadata before discard");
  }

  auto io = std::make_shared<FaultIo>(0, 0, 0);
  auto store = OpenStore(fixture.path, io);
  const auto exposed = store->current();
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == 0 &&
              action == eufs::journal::RecoveryAction::kDiscarded &&
              io->pwrite_calls() == 1 && io->sync_calls() == 1,
          detail.c_str());
  Require(store->current().generation == exposed.generation + 1U &&
              store->current().head == exposed.head &&
              store->current().tail == exposed.head &&
              store->current().used_blocks == 0 &&
              store->current().next_transaction_id ==
                  exposed.next_transaction_id,
          "discard changed the clean-control accounting rules");
  store.reset();

  Require(ReadTransactionRing(fixture, body) == ring_before,
          "discard erased transaction ring bytes");
  RequireEmptyFileSemantics(fixture);
  RequireCleanControl(fixture.path, exposed.generation + 1U, exposed.head,
                      exposed.next_transaction_id);
  unlink(fixture.path.c_str());
}

void TestCommittedReplayProducesRealHelloAndCheckpoint() {
  auto fixture = CreateFirstWriteFixture();
  const auto body = StageFirstWrite(fixture, true);
  const auto ring_before = ReadTransactionRing(fixture, body);
  auto io = std::make_shared<FaultIo>(0, 0, 0);
  auto store = OpenStore(fixture.path, io);
  const auto committed = store->current();
  eufs::journal::RecoveryAction action{};
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == 0 &&
              action ==
                  eufs::journal::RecoveryAction::kReplayedAndCheckpointed &&
              io->pwrite_calls() ==
                  fixture.plan.metadata_after_images.size() + 1U &&
              io->sync_calls() == 2,
          detail.c_str());
  Require(!store->validated_transaction().has_value(),
          "checkpoint retained a replay-capable transaction");
  store.reset();

  Require(ReadTransactionRing(fixture, body) == ring_before,
          "checkpoint erased committed ring bytes");
  RequireHelloSemantics(fixture);
  RequireCleanControl(fixture.path, committed.generation + 1U, committed.head,
                      committed.next_transaction_id);

  io = std::make_shared<FaultIo>(0, 0, 0);
  store = OpenStore(fixture.path, io);
  action = eufs::journal::RecoveryAction::kDiscarded;
  Require(store->ResolveRecovery(&action, &detail) == 0 &&
              action == eufs::journal::RecoveryAction::kNoAction &&
              io->pwrite_calls() == 0 && io->sync_calls() == 0,
          "second recovery did not converge to a no-op");
  store.reset();
  unlink(fixture.path.c_str());
}

void TestPartialHomeReplayRetriesIdempotently() {
  auto fixture = CreateFirstWriteFixture();
  StageFirstWrite(fixture, true);
  auto io = std::make_shared<FaultIo>(2, 0, 0);
  auto store = OpenStore(fixture.path, io);
  const auto control_before = store->current();
  eufs::journal::RecoveryAction action =
      eufs::journal::RecoveryAction::kDiscarded;
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == -EIO &&
              action == eufs::journal::RecoveryAction::kDiscarded &&
              store->reload_required() && io->pwrite_calls() == 2 &&
              io->sync_calls() == 0 &&
              store->current().generation == control_before.generation,
          "partial home replay advanced recovery authority");
  const auto first = fixture.plan.metadata_after_images.begin();
  const auto second = std::next(first);
  Require(ReadBlock(fixture.path, first->first) == first->second &&
              ReadBlock(fixture.path, second->first) ==
                  fixture.plan.before_images.at(second->first),
          "partial replay fixture did not stop between home blocks");
  store.reset();

  FinishCommittedRecovery(fixture);
  RequireHelloSemantics(fixture);
  unlink(fixture.path.c_str());
}

void TestHomeSyncUnknownKeepsCommittedRecovery() {
  auto fixture = CreateFirstWriteFixture();
  StageFirstWrite(fixture, true);
  auto io = std::make_shared<FaultIo>(0, 0, 1);
  auto store = OpenStore(fixture.path, io);
  const auto control_before = store->current();
  eufs::journal::RecoveryAction action =
      eufs::journal::RecoveryAction::kNoAction;
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == -EIO &&
              action == eufs::journal::RecoveryAction::kNoAction &&
              store->reload_required() &&
              io->pwrite_calls() ==
                  fixture.plan.metadata_after_images.size() &&
              io->sync_calls() == 1 &&
              store->current().generation == control_before.generation,
          "home sync uncertainty published a checkpoint");
  store.reset();

  auto reopened = OpenStore(fixture.path);
  Require(reopened->current().used_blocks == 4,
          "home sync uncertainty lost the committed journal range");
  reopened.reset();
  FinishCommittedRecovery(fixture);
  RequireHelloSemantics(fixture);
  unlink(fixture.path.c_str());
}

void TestCheckpointWriteFailureSelectsOldCommittedControl() {
  auto fixture = CreateFirstWriteFixture();
  StageFirstWrite(fixture, true);
  auto io = std::make_shared<FaultIo>(0, 3, 0);
  auto store = OpenStore(fixture.path, io);
  eufs::journal::RecoveryAction action =
      eufs::journal::RecoveryAction::kDiscarded;
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == -EIO &&
              action == eufs::journal::RecoveryAction::kDiscarded &&
              store->reload_required() && io->pwrite_calls() == 4 &&
              io->sync_calls() == 1,
          "partial checkpoint write did not latch recovery Store");
  store.reset();

  auto reopened = OpenStore(fixture.path);
  Require(reopened->current().used_blocks == 4,
          "torn clean control displaced the old committed control");
  reopened.reset();
  FinishCommittedRecovery(fixture);
  RequireHelloSemantics(fixture);
  unlink(fixture.path.c_str());
}

void TestCheckpointSyncUnknownCanSelectNewCleanControl() {
  auto fixture = CreateFirstWriteFixture();
  StageFirstWrite(fixture, true);
  auto io = std::make_shared<FaultIo>(0, 0, 2);
  auto store = OpenStore(fixture.path, io);
  eufs::journal::RecoveryAction action =
      eufs::journal::RecoveryAction::kDiscarded;
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == -EIO &&
              action == eufs::journal::RecoveryAction::kDiscarded &&
              store->reload_required() && io->pwrite_calls() == 3 &&
              io->sync_calls() == 2,
          "checkpoint sync uncertainty was reported as success");
  store.reset();

  io = std::make_shared<FaultIo>(0, 0, 0);
  store = OpenStore(fixture.path, io);
  Require(store->current().used_blocks == 0,
          "durable clean control was not selected after uncertain sync");
  action = eufs::journal::RecoveryAction::kDiscarded;
  Require(store->ResolveRecovery(&action, &detail) == 0 &&
              action == eufs::journal::RecoveryAction::kNoAction &&
              io->pwrite_calls() == 0 && io->sync_calls() == 0,
          "new clean control caused unnecessary replay");
  store.reset();
  RequireHelloSemantics(fixture);
  unlink(fixture.path.c_str());
}

void TestUncommittedCheckpointFailureRetriesDiscard() {
  auto fixture = CreateFirstWriteFixture();
  StageFirstWrite(fixture, false);
  auto io = std::make_shared<FaultIo>(0, 1, 0);
  auto store = OpenStore(fixture.path, io);
  eufs::journal::RecoveryAction action =
      eufs::journal::RecoveryAction::kReplayedAndCheckpointed;
  std::string detail;
  Require(store->ResolveRecovery(&action, &detail) == -EIO &&
              action ==
                  eufs::journal::RecoveryAction::kReplayedAndCheckpointed &&
              store->reload_required() && io->pwrite_calls() == 2 &&
              io->sync_calls() == 0,
          "failed uncommitted cleanup advanced its output");
  store.reset();

  store = OpenStore(fixture.path);
  Require(store->current().used_blocks == 4,
          "torn discard control displaced the uncommitted transaction");
  action = eufs::journal::RecoveryAction::kNoAction;
  Require(store->ResolveRecovery(&action, &detail) == 0 &&
              action == eufs::journal::RecoveryAction::kDiscarded,
          detail.c_str());
  store.reset();
  RequireEmptyFileSemantics(fixture);
  unlink(fixture.path.c_str());
}

}  // namespace

int main() {
  TestEmptyAndActiveWriterGuards();
  TestUncommittedDiscardPublishesOnlyCleanControl();
  TestCommittedReplayProducesRealHelloAndCheckpoint();
  TestPartialHomeReplayRetriesIdempotently();
  TestHomeSyncUnknownKeepsCommittedRecovery();
  TestCheckpointWriteFailureSelectsOldCommittedControl();
  TestCheckpointSyncUnknownCanSelectNewCleanControl();
  TestUncommittedCheckpointFailureRetriesDiscard();
  std::cout << "empty=noop uncommitted=discard committed=home_sync_checkpoint "
               "retry=idempotent checkpoint_unknown=reselect semantic=hello\n";
  std::cout << "PASS: journal recovery executor test\n";
  return 0;
}
