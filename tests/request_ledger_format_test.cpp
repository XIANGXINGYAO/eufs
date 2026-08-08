#include "metadata/ondisk_format.h"
#include "object/request_ledger_format.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

constexpr std::size_t kChecksumOffset = 92;

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

void PutLe16(std::uint8_t* output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void PutLe32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void PutLe64(std::uint8_t* output, std::uint64_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void RefreshChecksum(eufs::object_store::RequestLedgerBytes* bytes) {
  PutLe32(bytes->data() + kChecksumOffset, 0);
  PutLe32(bytes->data() + kChecksumOffset,
          eufs::ondisk::Crc32c(bytes->data(), bytes->size()));
}

eufs::object_store::RequestLedgerRecord CommittedRecord() {
  eufs::object_store::RequestLedgerRecord record;
  record.operation =
      eufs::object_store::MutationOperation::kCreateIfAbsent;
  record.result_kind = eufs::object_store::LedgerResultKind::kCommitted;
  for (std::size_t index = 0; index < record.request_id.size(); ++index) {
    record.request_id[index] = static_cast<std::uint8_t>(index + 1U);
  }
  for (std::size_t index = 0; index < record.fingerprint.size(); ++index) {
    record.fingerprint[index] = static_cast<std::uint8_t>(0x20U + index);
  }
  record.result_code = eufs::object_store::LedgerResultCode::kOk;
  record.committed_inode = 7;
  record.committed_generation = 11;
  record.sequence = 1;
  return record;
}

void TestGoldenBytesAndRoundTrip() {
  const auto record = CommittedRecord();
  eufs::object_store::RequestLedgerBytes encoded{};
  std::string error;
  Require(eufs::object_store::EncodeRequestLedgerRecord(record, &encoded,
                                                         &error),
          error.c_str());

  eufs::object_store::RequestLedgerBytes expected{};
  expected[0] = 'E';
  expected[1] = 'U';
  expected[2] = 'L';
  expected[3] = 'G';
  PutLe16(expected.data() + 4, 1);
  expected[6] = 1;
  expected[7] = 1;
  std::copy(record.request_id.begin(), record.request_id.end(),
            expected.begin() + 8);
  std::copy(record.fingerprint.begin(), record.fingerprint.end(),
            expected.begin() + 24);
  PutLe32(expected.data() + 56, 1);
  PutLe32(expected.data() + 60, 7);
  PutLe64(expected.data() + 64, 11);
  PutLe64(expected.data() + 84, 1);
  // 该 golden vector 的 CRC32C 固定为 little-endian 0x3e36e876。
  expected[92] = 0x76;
  expected[93] = 0xe8;
  expected[94] = 0x36;
  expected[95] = 0x3e;
  Require(encoded == expected, "ledger encoding changed the v1 golden bytes");

  eufs::object_store::RequestLedgerRecord decoded;
  Require(eufs::object_store::DecodeRequestLedgerRecord(
              encoded, 1, &decoded, &error) ==
              eufs::object_store::LedgerDecodeStatus::kRecord &&
              decoded.operation == record.operation &&
              decoded.result_kind == record.result_kind &&
              decoded.request_id == record.request_id &&
              decoded.fingerprint == record.fingerprint &&
              decoded.result_code == record.result_code &&
              decoded.committed_inode == 7 &&
              decoded.committed_generation == 11 &&
              decoded.current_inode == 0 &&
              decoded.current_generation == 0 && decoded.sequence == 1,
          "ledger round trip changed a committed result");
}

void TestNotAppliedRoundTrip() {
  auto record = CommittedRecord();
  record.operation =
      eufs::object_store::MutationOperation::kReplaceIfVersion;
  record.result_kind = eufs::object_store::LedgerResultKind::kNotApplied;
  record.result_code =
      eufs::object_store::LedgerResultCode::kVersionMismatch;
  record.committed_inode = 0;
  record.committed_generation = 0;
  record.current_inode = 9;
  record.current_generation = 13;
  record.sequence = 2;

  eufs::object_store::RequestLedgerBytes encoded{};
  eufs::object_store::RequestLedgerRecord decoded;
  std::string error;
  Require(eufs::object_store::EncodeRequestLedgerRecord(record, &encoded,
                                                         &error) &&
              eufs::object_store::DecodeRequestLedgerRecord(
                  encoded, 2, &decoded, &error) ==
                  eufs::object_store::LedgerDecodeStatus::kRecord &&
              decoded.result_kind ==
                  eufs::object_store::LedgerResultKind::kNotApplied &&
              decoded.current_inode == 9 &&
              decoded.current_generation == 13,
          "ledger round trip changed a known rejection");
}

void RequireCorruptWithoutOutputChange(
    const eufs::object_store::RequestLedgerBytes& bytes,
    std::uint64_t expected_sequence, const char* message) {
  auto output = CommittedRecord();
  output.sequence = 99;
  const auto original = output;
  std::string error;
  const auto status = eufs::object_store::DecodeRequestLedgerRecord(
      bytes, expected_sequence, &output, &error);
  Require(status == eufs::object_store::LedgerDecodeStatus::kCorrupt &&
              output.sequence == original.sequence &&
              output.request_id == original.request_id && !error.empty(),
          message);
}

void TestEmptyAndCorruption() {
  eufs::object_store::RequestLedgerBytes empty{};
  auto output = CommittedRecord();
  output.sequence = 99;
  std::string error;
  Require(eufs::object_store::DecodeRequestLedgerRecord(
              empty, 1, &output, &error) ==
              eufs::object_store::LedgerDecodeStatus::kEmpty &&
              output.sequence == 99,
          "all-zero slot was not an empty non-mutating decode");

  eufs::object_store::RequestLedgerBytes valid{};
  Require(eufs::object_store::EncodeRequestLedgerRecord(
              CommittedRecord(), &valid, &error),
          error.c_str());

  auto damaged = valid;
  damaged[0] ^= 1U;
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1, "bad magic was accepted");

  damaged = valid;
  PutLe16(damaged.data() + 4, 2);
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1, "bad version was accepted");

  damaged = valid;
  damaged[6] = 99;
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1,
                                    "bad operation enum was accepted");

  damaged = valid;
  damaged[7] = 99;
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1,
                                    "bad result kind was accepted");

  damaged = valid;
  PutLe32(damaged.data() + 56, 99);
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1,
                                    "bad result code was accepted");

  damaged = valid;
  PutLe64(damaged.data() + 84, 2);
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1,
                                    "wrong slot sequence was accepted");

  damaged = valid;
  damaged[24] ^= 1U;
  RequireCorruptWithoutOutputChange(damaged, 1, "bad CRC was accepted");

  damaged = valid;
  damaged[96] = 1;
  RefreshChecksum(&damaged);
  RequireCorruptWithoutOutputChange(damaged, 1,
                                    "nonzero reserved bytes were accepted");
}

void TestEncodeRejectsInconsistentFactsWithoutChangingOutput() {
  auto record = CommittedRecord();
  record.result_kind = eufs::object_store::LedgerResultKind::kNotApplied;
  eufs::object_store::RequestLedgerBytes output{};
  output.fill(0xa5);
  const auto original = output;
  std::string error;
  Require(!eufs::object_store::EncodeRequestLedgerRecord(record, &output,
                                                          &error) &&
              output == original && !error.empty(),
          "encoder accepted contradictory result fields or changed output");

  record = CommittedRecord();
  record.request_id.fill(0);
  Require(!eufs::object_store::EncodeRequestLedgerRecord(record, &output,
                                                          &error) &&
              output == original,
          "encoder accepted an unset request id");
}

}  // namespace

int main() {
  TestGoldenBytesAndRoundTrip();
  TestNotAppliedRoundTrip();
  TestEmptyAndCorruption();
  TestEncodeRejectsInconsistentFactsWithoutChangingOutput();
  std::cout << "request_ledger_format_test: PASS\n";
  return 0;
}
