#include "journal/journal_control_store.h"
#include "journal/ondisk_journal.h"
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

struct FirstWriteFixture {
  std::string path;
  eufs::ondisk::Superblock superblock;
  eufs::metadata::FirstBlockWritePlan plan;
};

FirstWriteFixture CreateFirstWriteFixture() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-ordered-stage-XXXXXX");
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
  FaultIo(std::size_t partial_pwrite_call, std::size_t failing_sync_call)
      : partial_pwrite_call_(partial_pwrite_call),
        failing_sync_call_(failing_sync_call) {}

  ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                 off_t offset) override {
    ++pwrite_calls_;
    if (fail_next_pwrite_) {
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
    if (fdatasync(fd) != 0) {
      return -1;
    }
    if (sync_calls_ == failing_sync_call_) {
      errno = EIO;
      return -1;
    }
    return 0;
  }

  std::size_t pwrite_calls() const { return pwrite_calls_; }
  std::size_t sync_calls() const { return sync_calls_; }

 private:
  std::size_t partial_pwrite_call_{0};
  std::size_t failing_sync_call_{0};
  std::size_t pwrite_calls_{0};
  std::size_t sync_calls_{0};
  bool fail_next_pwrite_{false};
};

class RecordingObserver final
    : public eufs::journal::DurableStageObserver {
 public:
  void OnDurableStage(eufs::journal::DurableStage stage) override {
    stages.push_back(stage);
  }

  std::vector<eufs::journal::DurableStage> stages;
};

void RequireControlsUnchanged(
    const std::string& path, const eufs::ondisk::Superblock& superblock,
    const eufs::ondisk::Block& control_a,
    const eufs::ondisk::Block& control_b) {
  Require(ReadBlock(path, superblock.journal.start_block) == control_a &&
              ReadBlock(path, superblock.journal.start_block + 1U) == control_b,
          "ordered transaction failure changed A/B control");
}

void TestFirstWriteStagesDataBodyThenExposure() {
  auto fixture = CreateFirstWriteFixture();
  const auto control_a =
      ReadBlock(fixture.path, fixture.superblock.journal.start_block);
  const auto control_b =
      ReadBlock(fixture.path, fixture.superblock.journal.start_block + 1U);

  eufs::ondisk::Block commit_sentinel{};
  commit_sentinel.fill(0xC7);
  WriteBlock(fixture.path, RingPhysicalBlock(fixture.superblock, 3),
             commit_sentinel);

  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(fixture.path, &store,
                                                    &detail) == 0,
          detail.c_str());
  eufs::journal::DurableJournalBody body;
  Require(store->WriteOrderedDataAndUnexposedBody(
              fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
              fixture.plan.before_images,
              fixture.plan.ordered_data_after_images,
              fixture.plan.metadata_after_images, &body, &detail) == 0,
          detail.c_str());
  Require(store->current().used_blocks == 0 && body.entry_count == 2,
          "durable data/body stage exposed control too early");

  Require(ReadBlock(fixture.path, fixture.plan.data_block) ==
              fixture.plan.ordered_data_after_images.at(
                  fixture.plan.data_block),
          "ordered data was not written to its home block");
  for (const auto& [block, after_image] :
       fixture.plan.metadata_after_images) {
    (void)after_image;
    Require(ReadBlock(fixture.path, block) ==
                fixture.plan.before_images.at(block),
            "metadata home block changed before COMMIT");
  }

  const auto descriptor_bytes =
      ReadBlock(fixture.path,
                RingPhysicalBlock(fixture.superblock,
                                  body.reservation.descriptor_ring_index));
  eufs::journal::DescriptorRecord descriptor;
  Require(eufs::journal::DecodeDescriptor(descriptor_bytes, &descriptor,
                                          &detail) &&
              descriptor.entries.size() == 2,
          detail.c_str());
  std::size_t index = 0;
  for (const auto& [home_block, metadata_after] :
       fixture.plan.metadata_after_images) {
    const auto& entry = descriptor.entries[index];
    Require(entry.home_block == home_block &&
                entry.payload_ring_index ==
                    body.reservation.payload_ring_indices[index] &&
                ReadBlock(fixture.path,
                          RingPhysicalBlock(fixture.superblock,
                                            entry.payload_ring_index)) ==
                    metadata_after,
            "journal payload is not the classified metadata after-image");
    ++index;
  }
  Require(ReadBlock(fixture.path,
                    RingPhysicalBlock(fixture.superblock,
                                      body.reservation.commit_ring_index)) ==
              commit_sentinel,
          "ordered transaction stage wrote the COMMIT slot");
  RequireControlsUnchanged(fixture.path, fixture.superblock, control_a,
                           control_b);

  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  store.reset();

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(fixture.path, &reader, &detail) == 0,
          detail.c_str());
  Require(reader->journal_control().generation == 1 &&
              reader->journal_control().head == 4 &&
              reader->journal_control().tail == 0 &&
              reader->journal_control().used_blocks == 4,
          "control exposure did not publish the staged four-block range");
  eufs::ondisk::InodeRecord inode;
  Require(reader->ReadInode(2, &inode, &detail) == 0 && inode.size == 0 &&
              inode.direct_blocks[0] == 0,
          "uncommitted exposure changed home inode metadata");
  reader.reset();
  Require(ReadBlock(fixture.path,
                    RingPhysicalBlock(fixture.superblock, 3)) ==
              commit_sentinel,
          "control exposure wrote the COMMIT slot");
  unlink(fixture.path.c_str());
}

void TestValidationAndStalePlanRejectBeforeIo() {
  auto fixture = CreateFirstWriteFixture();
  auto io = std::make_shared<FaultIo>(0, 0);
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(fixture.path, &store,
                                                    &detail, io) == 0,
          detail.c_str());
  auto missing_before = fixture.plan.before_images;
  missing_before.erase(fixture.plan.data_block);
  eufs::journal::DurableJournalBody body;
  Require(store->WriteOrderedDataAndUnexposedBody(
              fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
              missing_before, fixture.plan.ordered_data_after_images,
              fixture.plan.metadata_after_images, &body, &detail) == -EINVAL &&
              io->pwrite_calls() == 0 && io->sync_calls() == 0,
          "incomplete classification reached image I/O");
  store.reset();

  const auto unchanged_data =
      ReadBlock(fixture.path, fixture.plan.data_block);
  WriteBlock(fixture.path, 2, fixture.plan.metadata_after_images.at(2));
  io = std::make_shared<FaultIo>(0, 0);
  Require(eufs::journal::JournalControlStore::Open(fixture.path, &store,
                                                    &detail, io) == 0,
          detail.c_str());
  Require(store->WriteOrderedDataAndUnexposedBody(
              fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
              fixture.plan.before_images,
              fixture.plan.ordered_data_after_images,
              fixture.plan.metadata_after_images, &body, &detail) == -ESTALE &&
              io->pwrite_calls() == 0 && io->sync_calls() == 0 &&
              !store->reload_required(),
          "changed allocation ownership reached image mutation");
  Require(ReadBlock(fixture.path, fixture.plan.data_block) == unchanged_data,
          "ownership conflict modified the planned data block");
  store.reset();

  auto foreign = CreateFirstWriteFixture();
  for (const auto& [block, expected] : fixture.plan.before_images) {
    WriteBlock(foreign.path, block, expected);
    Require(ReadBlock(foreign.path, block) == expected,
            "cross-image fixture does not have matching home bytes");
  }
  io = std::make_shared<FaultIo>(0, 0);
  Require(eufs::journal::JournalControlStore::Open(foreign.path, &store,
                                                    &detail, io) == 0,
          detail.c_str());
  Require(store->WriteOrderedDataAndUnexposedBody(
              fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
              fixture.plan.before_images,
              fixture.plan.ordered_data_after_images,
              fixture.plan.metadata_after_images, &body, &detail) == -ESTALE &&
              io->pwrite_calls() == 0 && io->sync_calls() == 0,
          "matching bytes from another filesystem passed identity binding");
  store.reset();
  unlink(foreign.path.c_str());
  unlink(fixture.path.c_str());
}

void TestCommittedTransactionReplaysAndCheckpoints() {
  auto fixture = CreateFirstWriteFixture();
  auto observer = std::make_shared<RecordingObserver>();
  std::unique_ptr<eufs::journal::JournalControlStore> store;
  std::string detail;
  Require(eufs::journal::JournalControlStore::Open(fixture.path, &store,
                                                    &detail, nullptr,
                                                    observer) == 0,
          detail.c_str());

  eufs::journal::DurableJournalBody body;
  Require(store->WriteOrderedDataAndUnexposedBody(
              fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
              fixture.plan.before_images,
              fixture.plan.ordered_data_after_images,
              fixture.plan.metadata_after_images, &body, &detail) == 0,
          detail.c_str());
  Require(store->ExposeDurableBody(&detail) == 0, detail.c_str());
  Require(store->WriteCommit(&detail) == 0, detail.c_str());
  Require(store->CompleteCommittedTransaction(&detail) == 0,
          detail.c_str());
  Require(store->current().used_blocks == 0 &&
              store->current().head == store->current().tail,
          "completed transaction did not publish a clean checkpoint");
  Require(observer->stages ==
              std::vector<eufs::journal::DurableStage>{
                  eufs::journal::DurableStage::kOrderedData,
                  eufs::journal::DurableStage::kJournalBody,
                  eufs::journal::DurableStage::kControlExposure,
                  eufs::journal::DurableStage::kCommit,
                  eufs::journal::DurableStage::kHomeBlocks,
                  eufs::journal::DurableStage::kCheckpoint},
          "durable stage observer did not preserve transaction ordering");
  store.reset();

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(fixture.path, &reader, &detail) == 0,
          detail.c_str());
  eufs::ondisk::InodeRecord inode;
  Require(reader->ReadInode(fixture.plan.inode_number, &inode, &detail) == 0,
          detail.c_str());
  Require(inode.size == 5 &&
              inode.direct_blocks[0] == fixture.plan.data_block &&
              ReadBlock(fixture.path, fixture.plan.data_block) ==
                  fixture.plan.ordered_data_after_images.at(
                      fixture.plan.data_block),
          "completed transaction did not publish data and metadata homes");
  Require(reader->journal_control().used_blocks == 0,
          "reopened reader observed a non-clean journal after checkpoint");
  reader.reset();
  unlink(fixture.path.c_str());
}

void TestFailureBoundariesStayUnexposed() {
  struct FailureCase {
    std::size_t partial_pwrite_call;
    std::size_t failing_sync_call;
    std::size_t expected_sync_calls;
  };
  for (const auto test : {FailureCase{1, 0, 0}, FailureCase{0, 1, 1},
                          FailureCase{2, 0, 1}, FailureCase{0, 2, 2}}) {
    auto fixture = CreateFirstWriteFixture();
    const auto control_a =
        ReadBlock(fixture.path, fixture.superblock.journal.start_block);
    const auto control_b =
        ReadBlock(fixture.path, fixture.superblock.journal.start_block + 1U);
    auto io = std::make_shared<FaultIo>(test.partial_pwrite_call,
                                        test.failing_sync_call);
    std::unique_ptr<eufs::journal::JournalControlStore> store;
    std::string detail;
    Require(eufs::journal::JournalControlStore::Open(fixture.path, &store,
                                                      &detail, io) == 0,
            detail.c_str());
    eufs::journal::DurableJournalBody output;
    output.entry_count = 99;
    Require(store->WriteOrderedDataAndUnexposedBody(
                fixture.plan.total_blocks, fixture.plan.filesystem_uuid,
                fixture.plan.before_images,
                fixture.plan.ordered_data_after_images,
                fixture.plan.metadata_after_images, &output, &detail) == -EIO &&
                store->reload_required() && output.entry_count == 99 &&
                !store->durable_body().has_value() &&
                io->sync_calls() == test.expected_sync_calls,
            "ordered transaction failure advanced in-memory authority");
    const std::size_t writes_after_failure = io->pwrite_calls();
    Require(store->ExposeDurableBody(&detail) == -EIO &&
                io->pwrite_calls() == writes_after_failure,
            "failed ordered transaction attempted control exposure");
    RequireControlsUnchanged(fixture.path, fixture.superblock, control_a,
                             control_b);
    for (const auto& [block, after_image] :
         fixture.plan.metadata_after_images) {
      (void)after_image;
      Require(ReadBlock(fixture.path, block) ==
                  fixture.plan.before_images.at(block),
              "failure path modified metadata home block");
    }
    store.reset();
    unlink(fixture.path.c_str());
  }
}

}  // namespace

int main() {
  TestFirstWriteStagesDataBodyThenExposure();
  TestValidationAndStalePlanRejectBeforeIo();
  TestCommittedTransactionReplaysAndCheckpoints();
  TestFailureBoundariesStayUnexposed();
  std::cout << "classification=data292_metadata2,3 ordering=data_body_control "
               "commit=untouched failures=latched\n";
  std::cout << "PASS: ordered first-write transaction stage test\n";
  return 0;
}
