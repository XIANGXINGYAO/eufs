#include "checker/consistency_checker.h"
#include "metadata/ondisk_format.h"
#include "object/object_backend.h"
#include "object/request_ledger_index.h"
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
#include <unistd.h>

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
    const auto result =
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

bool PwriteAll(int fd, const std::uint8_t* input, std::size_t size,
               std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result =
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

ImageFixture CreateImage(bool with_ledger) {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-ledger-index-XXXXXX");
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
  options.request_ledger_entries = with_ledger ? 64U : 0U;
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

eufs::object_store::RequestLedgerRecord Record(std::uint64_t sequence,
                                                std::uint8_t marker) {
  eufs::object_store::RequestLedgerRecord record;
  record.operation =
      eufs::object_store::MutationOperation::kCreateIfAbsent;
  record.result_kind = eufs::object_store::LedgerResultKind::kCommitted;
  record.request_id.fill(marker);
  for (std::size_t index = 0; index < record.fingerprint.size(); ++index) {
    record.fingerprint[index] =
        static_cast<std::uint8_t>(marker + index);
  }
  record.result_code = eufs::object_store::LedgerResultCode::kOk;
  record.committed_inode = static_cast<std::uint32_t>(10U + marker);
  record.committed_generation = 1;
  record.sequence = sequence;
  return record;
}

void WriteSlot(const std::string& path, std::size_t slot,
               const eufs::object_store::RequestLedgerBytes& bytes) {
  auto reader = OpenReader(path);
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  std::string detail;
  const std::string ledger_path =
      std::string("/") +
      std::string(eufs::object_store::kRequestLedgerName);
  Require(reader->ResolvePath(ledger_path, &inode_number, &inode, &detail) == 0,
          detail.c_str());
  const std::uint32_t logical = static_cast<std::uint32_t>(
      slot * eufs::object_store::kRequestLedgerRecordSize /
      eufs::ondisk::kBlockSize);
  std::uint32_t physical = 0;
  Require(reader->MapLogicalBlock(inode, logical, &physical, &detail) == 0,
          detail.c_str());
  reader.reset();

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
          "could not write request ledger slot fixture");
  close(fd);
}

void WriteRecord(const std::string& path, std::size_t slot,
                 const eufs::object_store::RequestLedgerRecord& record) {
  eufs::object_store::RequestLedgerBytes bytes{};
  std::string detail;
  Require(eufs::object_store::EncodeRequestLedgerRecord(record, &bytes,
                                                         &detail),
          detail.c_str());
  WriteSlot(path, slot, bytes);
}

int Scan(const std::string& path,
         eufs::object_store::RequestLedgerIndex* output,
         std::string* detail) {
  auto reader = OpenReader(path);
  return eufs::object_store::ScanRequestLedger(*reader, output, detail);
}

int OpenBackend(const std::string& path,
                std::unique_ptr<eufs::object_store::ObjectBackend>* output,
                std::string* detail) {
  eufs::journal::RecoveryAction action{};
  eufs::object_store::ObjectBackendOptions options;
  return eufs::object_store::ObjectBackend::Open(
      path, options, output, &action, detail);
}

const eufs::checker::CheckIssue* FindLedgerIssue(
    const eufs::checker::ConsistencyReport& report,
    eufs::checker::IssueCode code, std::uint64_t slot) {
  const auto it = std::find_if(
      report.issues.begin(), report.issues.end(),
      [code, slot](const eufs::checker::CheckIssue& issue) {
        return issue.code == code && issue.ledger_slot == slot;
      });
  return it == report.issues.end() ? nullptr : &*it;
}

void TestEmptyAndValidPrefix() {
  auto fixture = CreateImage(true);
  eufs::object_store::RequestLedgerIndex index;
  std::string detail;
  Require(Scan(fixture.path, &index, &detail) == 0 &&
              index.capacity() == 64 && index.size() == 0 &&
              index.next_sequence() == 1 && !index.full(),
          "empty request ledger scan is incorrect");

  const auto first = Record(1, 1);
  auto second = Record(2, 2);
  second.operation =
      eufs::object_store::MutationOperation::kReplaceIfVersion;
  second.result_kind = eufs::object_store::LedgerResultKind::kNotApplied;
  second.result_code =
      eufs::object_store::LedgerResultCode::kVersionMismatch;
  second.committed_inode = 0;
  second.committed_generation = 0;
  second.current_inode = 20;
  second.current_generation = 7;
  WriteRecord(fixture.path, 0, first);
  WriteRecord(fixture.path, 1, second);

  Require(Scan(fixture.path, &index, &detail) == 0 && index.size() == 2 &&
              index.next_sequence() == 3 &&
              index.Find(first.request_id) != nullptr &&
              index.Find(first.request_id)->committed_inode ==
                  first.committed_inode &&
              index.Find(second.request_id) != nullptr &&
              index.Find(second.request_id)->current_generation == 7,
          "valid request ledger prefix did not rebuild the index");
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  Require(OpenBackend(fixture.path, &backend, &detail) == 0 &&
              backend != nullptr && backend->usable(),
          "ObjectBackend did not publish after a valid ledger startup scan");
}

void TestHoleDuplicateAndCorruption() {
  std::string detail;
  eufs::object_store::RequestLedgerIndex index;

  auto baseline = CreateImage(true);
  const auto baseline_record = Record(1, 9);
  WriteRecord(baseline.path, 0, baseline_record);
  Require(Scan(baseline.path, &index, &detail) == 0 && index.size() == 1,
          "could not build baseline index for failure contract");

  auto hole = CreateImage(true);
  WriteRecord(hole.path, 0, Record(1, 1));
  WriteRecord(hole.path, 2, Record(3, 3));
  Require(Scan(hole.path, &index, &detail) == -EUCLEAN &&
              detail.find("after the first empty slot") != std::string::npos &&
              index.size() == 1 &&
              index.Find(baseline_record.request_id) != nullptr,
          "request ledger accepted a record after an empty slot");

  auto duplicate = CreateImage(true);
  const auto original = Record(1, 4);
  auto repeated_id = Record(2, 5);
  repeated_id.request_id = original.request_id;
  WriteRecord(duplicate.path, 0, original);
  WriteRecord(duplicate.path, 1, repeated_id);
  Require(Scan(duplicate.path, &index, &detail) == -EUCLEAN &&
              detail.find("duplicate request id") != std::string::npos &&
              index.size() == 1 &&
              index.Find(baseline_record.request_id) != nullptr,
          "request ledger accepted a duplicate request id");

  auto corrupt = CreateImage(true);
  eufs::object_store::RequestLedgerBytes damaged{};
  Require(eufs::object_store::EncodeRequestLedgerRecord(
              Record(1, 6), &damaged, &detail),
          detail.c_str());
  damaged[24] ^= 1U;
  WriteSlot(corrupt.path, 0, damaged);
  Require(Scan(corrupt.path, &index, &detail) == -EUCLEAN &&
              detail.find("checksum") != std::string::npos &&
              index.size() == 1 &&
              index.Find(baseline_record.request_id) != nullptr,
          "request ledger accepted a damaged record");
  std::unique_ptr<eufs::object_store::ObjectBackend> backend;
  Require(OpenBackend(corrupt.path, &backend, &detail) == -EUCLEAN &&
              backend == nullptr,
          "ObjectBackend published despite a corrupt request ledger");
}

void TestFeatureAndIdentityBoundaries() {
  std::string detail;
  eufs::object_store::RequestLedgerIndex index;

  auto legacy = CreateImage(false);
  Require(Scan(legacy.path, &index, &detail) == -EOPNOTSUPP &&
              detail.find("does not declare") != std::string::npos,
          "scanner silently accepted an image without the ledger feature");

  auto unknown = CreateImage(false);
  const int fd = open(unknown.path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "could not open unknown-feature fixture");
  eufs::ondisk::Block superblock_bytes{};
  Require(PreadAll(fd, superblock_bytes.data(), superblock_bytes.size(), 0),
          "could not read unknown-feature superblock");
  eufs::ondisk::Superblock superblock;
  Require(eufs::ondisk::DecodeSuperblock(superblock_bytes, &superblock,
                                         &detail),
          detail.c_str());
  superblock.feature_incompat = 1ULL << 63U;
  Require(eufs::ondisk::EncodeSuperblock(superblock, &superblock_bytes,
                                         &detail) &&
              PwriteAll(fd, superblock_bytes.data(), superblock_bytes.size(),
                        0) &&
              fdatasync(fd) == 0,
          "could not write unknown-feature superblock");
  close(fd);
  std::unique_ptr<eufs::storage::ImageReader> rejected;
  Require(eufs::storage::ImageReader::Open(unknown.path, &rejected, &detail) ==
              -EOPNOTSUPP,
          "ImageReader accepted an unknown incompatible feature");

  auto wrong_identity = CreateImage(true);
  auto reader = OpenReader(wrong_identity.path);
  const std::uint32_t directory_block =
      reader->superblock().data.start_block;
  reader.reset();
  eufs::ondisk::Block directory{};
  eufs::ondisk::DirectoryEntry false_ledger;
  false_ledger.inode = 3;
  false_ledger.file_type = eufs::ondisk::DirectoryFileType::kRegular;
  false_ledger.name.assign(eufs::object_store::kRequestLedgerName);
  Require(eufs::ondisk::EncodeDirectoryEntry(
              false_ledger,
              static_cast<std::uint16_t>(eufs::ondisk::kBlockSize),
              directory.data(), directory.size(), &detail),
          detail.c_str());
  const int identity_fd =
      open(wrong_identity.path.c_str(), O_RDWR | O_CLOEXEC);
  Require(identity_fd >= 0 &&
              PwriteAll(identity_fd, directory.data(), directory.size(),
                        static_cast<std::uint64_t>(directory_block) *
                            eufs::ondisk::kBlockSize) &&
              fdatasync(identity_fd) == 0,
          "could not write false ledger identity");
  close(identity_fd);
  Require(Scan(wrong_identity.path, &index, &detail) == -EUCLEAN,
          "scanner accepted a ledger path pointing away from inode 2");
  eufs::checker::ConsistencyReport identity_report;
  Require(eufs::checker::CheckImage(wrong_identity.path, &identity_report,
                                    &detail) == 0 &&
              identity_report.request_ledger_scan_complete &&
              FindLedgerIssue(
                  identity_report,
                  eufs::checker::IssueCode::kRequestLedgerIdentityInvalid,
                  0) != nullptr,
          "eufsck did not separate ledger identity failure from slot evidence");
}

void TestCheckerCollectsIndependentLedgerContradictions() {
  auto healthy = CreateImage(true);
  eufs::checker::ConsistencyReport healthy_report;
  std::string detail;
  Require(eufs::checker::CheckImage(healthy.path, &healthy_report, &detail) ==
              0 &&
              healthy_report.request_ledger_feature_enabled &&
              healthy_report.request_ledger_scan_complete &&
              healthy_report.request_ledger_capacity == 64 &&
              healthy_report.request_ledger_slots_scanned == 64 &&
              healthy_report.request_ledger_valid_records == 0 &&
              healthy_report.request_ledger_empty_slots == 64,
          "eufsck did not completely inspect an empty request ledger");

  auto damaged = CreateImage(true);
  const auto first = Record(1, 1);
  WriteRecord(damaged.path, 0, first);

  eufs::object_store::RequestLedgerBytes corrupt{};
  Require(eufs::object_store::EncodeRequestLedgerRecord(
              Record(2, 2), &corrupt, &detail),
          detail.c_str());
  corrupt[24] ^= 1U;
  WriteSlot(damaged.path, 1, corrupt);

  // slot 3 保持全零；slot 4 同时构成“空洞后记录”和“重复 Request-ID”。
  auto duplicate_after_hole = Record(4, 4);
  duplicate_after_hole.request_id = first.request_id;
  WriteRecord(damaged.path, 3, duplicate_after_hole);

  eufs::checker::ConsistencyReport report;
  Require(eufs::checker::CheckImage(damaged.path, &report, &detail) == 0,
          detail.c_str());
  Require(report.request_ledger_scan_complete &&
              report.request_ledger_slots_scanned == 64 &&
              report.request_ledger_valid_records == 2 &&
              report.request_ledger_empty_slots == 61,
          "eufsck stopped ledger evidence collection after local corruption");
  Require(FindLedgerIssue(
              report, eufs::checker::IssueCode::kRequestLedgerSlotCorrupt,
              2) != nullptr &&
              FindLedgerIssue(
                  report,
                  eufs::checker::IssueCode::kRequestLedgerRecordAfterHole,
                  4) != nullptr &&
              FindLedgerIssue(
                  report,
                  eufs::checker::IssueCode::kRequestLedgerDuplicateRequestId,
                  4) != nullptr,
          "eufsck did not retain all independent ledger contradictions");
}

}  // namespace

int main() {
  TestEmptyAndValidPrefix();
  TestHoleDuplicateAndCorruption();
  TestFeatureAndIdentityBoundaries();
  TestCheckerCollectsIndependentLedgerContradictions();
  std::cout << "request_ledger_index_test: PASS\n";
  return 0;
}
