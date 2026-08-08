#include "object/request_ledger_plan.h"
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
    const ssize_t result =
        pwrite(fd, input + completed, size - completed,
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

struct ImageFixture {
  std::string path;

  ImageFixture() = default;
  ImageFixture(const ImageFixture&) = delete;
  ImageFixture& operator=(const ImageFixture&) = delete;
  ImageFixture(ImageFixture&& other) noexcept : path(std::move(other.path)) {
    other.path.clear();
  }
  ~ImageFixture() {
    if (!path.empty()) {
      unlink(path.c_str());
    }
  }
};

ImageFixture CreateImage(std::uint32_t ledger_entries = 64) {
  std::array<char, 72> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-ledger-plan-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  ImageFixture fixture;
  fixture.path = path_template.data();
  eufs::storage::MkfsOptions options;
  options.image_path = fixture.path;
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  options.request_ledger_entries = ledger_entries;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());
  return fixture;
}

std::unique_ptr<eufs::storage::ImageReader> OpenReader(
    const std::string& path) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  return reader;
}

eufs::object_store::RequestLedgerRecord Record(std::uint8_t marker) {
  eufs::object_store::RequestLedgerRecord record;
  record.operation =
      eufs::object_store::MutationOperation::kCreateIfAbsent;
  record.result_kind = eufs::object_store::LedgerResultKind::kCommitted;
  record.request_id.fill(marker);
  for (std::size_t index = 0; index < record.fingerprint.size(); ++index) {
    record.fingerprint[index] = static_cast<std::uint8_t>(marker + index);
  }
  record.result_code = eufs::object_store::LedgerResultCode::kOk;
  record.committed_inode = static_cast<std::uint32_t>(10U + marker);
  record.committed_generation = 1;
  return record;
}

struct LedgerView {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  eufs::object_store::RequestLedgerIndex index;
};

LedgerView OpenLedger(const std::string& path) {
  LedgerView view;
  view.reader = OpenReader(path);
  std::string detail;
  Require(eufs::object_store::ScanRequestLedger(*view.reader, &view.index,
                                                 &detail) == 0,
          detail.c_str());
  return view;
}

void WriteSlot(const std::string& path,
               const eufs::storage::ImageReader& reader, std::size_t slot,
               const eufs::object_store::RequestLedgerBytes& bytes) {
  const std::string ledger_path =
      std::string("/") +
      std::string(eufs::object_store::kRequestLedgerName);
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  Require(reader.ResolvePath(ledger_path, &inode_number, &inode, &detail) == 0,
          detail.c_str());
  const std::uint32_t logical = static_cast<std::uint32_t>(
      slot * eufs::object_store::kRequestLedgerRecordSize /
      eufs::ondisk::kBlockSize);
  std::uint32_t physical = 0;
  Require(reader.MapLogicalBlock(inode, logical, &physical, &detail) == 0,
          detail.c_str());
  const std::uint64_t inside =
      (slot * eufs::object_store::kRequestLedgerRecordSize) %
      eufs::ondisk::kBlockSize;

  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0 &&
              PwriteAll(fd, bytes.data(), bytes.size(),
                        static_cast<std::uint64_t>(physical) *
                                eufs::ondisk::kBlockSize +
                            inside) &&
              fdatasync(fd) == 0,
          "could not write request ledger test slot");
  close(fd);
}

void WriteRecord(const std::string& path,
                 const eufs::storage::ImageReader& reader, std::size_t slot,
                 std::uint8_t marker) {
  auto record = Record(marker);
  record.sequence = slot + 1U;
  eufs::object_store::RequestLedgerBytes bytes{};
  std::string detail;
  Require(eufs::object_store::EncodeRequestLedgerRecord(record, &bytes,
                                                         &detail),
          detail.c_str());
  WriteSlot(path, reader, slot, bytes);
}

eufs::ondisk::Block FilledBlock(std::uint8_t value) {
  eufs::ondisk::Block block{};
  block.fill(value);
  return block;
}

void TestSlotPlacementAndBlockBoundary() {
  auto fixture = CreateImage();
  {
    auto seed = OpenReader(fixture.path);
    for (std::size_t slot = 0; slot < 31; ++slot) {
      WriteRecord(fixture.path, *seed, slot,
                  static_cast<std::uint8_t>(slot + 1U));
    }
  }

  auto view = OpenLedger(fixture.path);
  eufs::object_store::RequestLedgerAppendPlan slot_32;
  std::string detail;
  Require(eufs::object_store::PrepareRequestLedgerAppend(
              *view.reader, view.index, Record(32), &slot_32, &detail) == 0,
          detail.c_str());
  Require(slot_32.sequence == 32 && slot_32.logical_block == 0,
          "sequence 32 did not target the last slot of ledger block zero");
  const std::size_t offset_32 =
      31U * eufs::object_store::kRequestLedgerRecordSize;
  Require(std::equal(slot_32.before_image.begin(),
                     slot_32.before_image.begin() + offset_32,
                     slot_32.after_image.begin()) &&
              slot_32.before_image[offset_32] == 0 &&
              slot_32.after_image[offset_32] != 0,
          "planner changed bytes before the selected ledger slot");

  eufs::object_store::RequestLedgerBytes encoded_32{};
  Require(eufs::object_store::EncodeRequestLedgerRecord(
              slot_32.record, &encoded_32, &detail),
          detail.c_str());
  WriteSlot(fixture.path, *view.reader, 31, encoded_32);
  view = OpenLedger(fixture.path);

  eufs::object_store::RequestLedgerAppendPlan slot_33;
  Require(eufs::object_store::PrepareRequestLedgerAppend(
              *view.reader, view.index, Record(33), &slot_33, &detail) == 0,
          detail.c_str());
  Require(slot_33.sequence == 33 && slot_33.logical_block == 1 &&
              slot_33.physical_block != slot_32.physical_block &&
              slot_33.before_image[0] == 0 && slot_33.after_image[0] != 0,
          "sequence 33 did not cross to the first slot of ledger block one");
}

void TestCapacityAndStaleIndexFailuresPreserveOutput() {
  auto full_fixture = CreateImage(32);
  {
    auto reader = OpenReader(full_fixture.path);
    for (std::size_t slot = 0; slot < 32; ++slot) {
      WriteRecord(full_fixture.path, *reader, slot,
                  static_cast<std::uint8_t>(slot + 1U));
    }
  }
  auto full = OpenLedger(full_fixture.path);
  eufs::object_store::RequestLedgerAppendPlan sentinel;
  sentinel.sequence = 777;
  sentinel.after_image[0] = 0xA5;
  std::string detail;
  Require(eufs::object_store::PrepareRequestLedgerAppend(
              *full.reader, full.index, Record(80), &sentinel, &detail) ==
              -ENOSPC &&
              sentinel.sequence == 777 && sentinel.after_image[0] == 0xA5,
          "full ledger changed planner output or returned the wrong error");

  auto stale_fixture = CreateImage();
  auto stale = OpenLedger(stale_fixture.path);
  WriteRecord(stale_fixture.path, *stale.reader, 0, 90);
  sentinel.sequence = 888;
  sentinel.after_image[0] = 0x5A;
  Require(eufs::object_store::PrepareRequestLedgerAppend(
              *stale.reader, stale.index, Record(91), &sentinel, &detail) ==
              -EUCLEAN &&
              sentinel.sequence == 888 && sentinel.after_image[0] == 0x5A,
          "occupied next slot did not reject a stale startup index safely");
}

void TestMergeStrongGuaranteeAndConflicts() {
  auto fixture = CreateImage();
  auto view = OpenLedger(fixture.path);
  eufs::object_store::RequestLedgerAppendPlan ledger;
  std::string detail;
  Require(eufs::object_store::PrepareRequestLedgerAppend(
              *view.reader, view.index, Record(100), &ledger, &detail) == 0,
          detail.c_str());

  std::map<std::uint32_t, eufs::ondisk::Block> before{{11, FilledBlock(1)}};
  std::map<std::uint32_t, eufs::ondisk::Block> ordered{{12, FilledBlock(2)}};
  std::map<std::uint32_t, eufs::ondisk::Block> metadata{{13, FilledBlock(3)}};
  Require(eufs::object_store::MergeRequestLedgerAppend(
              ledger, ledger.total_blocks, ledger.filesystem_uuid, &before,
              &ordered, &metadata, &detail) == 0,
          detail.c_str());
  Require(before.at(ledger.physical_block) == ledger.before_image &&
              metadata.at(ledger.physical_block) == ledger.after_image &&
              ordered.count(ledger.physical_block) == 0 &&
              ordered.at(12) == FilledBlock(2),
          "ledger was not merged exclusively as journal metadata");

  for (int conflict_kind = 0; conflict_kind < 3; ++conflict_kind) {
    std::map<std::uint32_t, eufs::ondisk::Block> conflict_before{
        {21, FilledBlock(4)}};
    std::map<std::uint32_t, eufs::ondisk::Block> conflict_ordered{
        {22, FilledBlock(5)}};
    std::map<std::uint32_t, eufs::ondisk::Block> conflict_metadata{
        {23, FilledBlock(6)}};
    auto* selected = conflict_kind == 0   ? &conflict_before
                     : conflict_kind == 1 ? &conflict_ordered
                                          : &conflict_metadata;
    selected->emplace(ledger.physical_block, FilledBlock(9));
    const auto old_before = conflict_before;
    const auto old_ordered = conflict_ordered;
    const auto old_metadata = conflict_metadata;
    Require(eufs::object_store::MergeRequestLedgerAppend(
                ledger, ledger.total_blocks, ledger.filesystem_uuid,
                &conflict_before, &conflict_ordered, &conflict_metadata,
                &detail) == -EINVAL &&
                conflict_before == old_before &&
                conflict_ordered == old_ordered &&
                conflict_metadata == old_metadata,
            "map conflict did not preserve all caller plans");
  }

  std::map<std::uint32_t, eufs::ondisk::Block> provenance_before;
  std::map<std::uint32_t, eufs::ondisk::Block> provenance_ordered;
  std::map<std::uint32_t, eufs::ondisk::Block> provenance_metadata;
  auto wrong_uuid = ledger.filesystem_uuid;
  wrong_uuid[0] ^= 0xFFU;
  Require(eufs::object_store::MergeRequestLedgerAppend(
              ledger, ledger.total_blocks, wrong_uuid, &provenance_before,
              &provenance_ordered, &provenance_metadata, &detail) == -EINVAL &&
              provenance_before.empty() && provenance_ordered.empty() &&
              provenance_metadata.empty(),
          "cross-image merge was not rejected before publishing maps");
}

}  // namespace

int main() {
  TestSlotPlacementAndBlockBoundary();
  TestCapacityAndStaleIndexFailuresPreserveOutput();
  TestMergeStrongGuaranteeAndConflicts();
  std::cout << "PASS: request ledger append planning and transaction merge\n";
  return 0;
}
