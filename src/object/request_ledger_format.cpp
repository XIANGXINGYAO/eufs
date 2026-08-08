#include "object/request_ledger_format.h"

#include "metadata/ondisk_format.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace eufs::object_store {
namespace {

constexpr std::array<std::uint8_t, 4> kLedgerMagic{'E', 'U', 'L', 'G'};
constexpr std::size_t kChecksumOffset = 92;
constexpr std::size_t kReservedOffset = 96;

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
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

std::uint16_t GetLe16(const std::uint8_t* input) {
  return static_cast<std::uint16_t>(input[0]) |
         (static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t GetLe64(const std::uint8_t* input) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
  }
  return value;
}

bool IsAllZero(const RequestLedgerBytes& bytes) {
  return std::all_of(bytes.begin(), bytes.end(),
                     [](std::uint8_t value) { return value == 0; });
}

bool HasNonzeroRequestId(const RequestId& request_id) {
  return std::any_of(request_id.begin(), request_id.end(),
                     [](std::uint8_t value) { return value != 0; });
}

bool ValidateRecord(const RequestLedgerRecord& value, std::string* error) {
  const bool create =
      value.operation == MutationOperation::kCreateIfAbsent;
  const bool replace =
      value.operation == MutationOperation::kReplaceIfVersion;
  if ((!create && !replace) || !HasNonzeroRequestId(value.request_id) ||
      value.sequence == 0) {
    SetError(error, "ledger operation, request id, or sequence is invalid");
    return false;
  }

  const bool committed = value.result_kind == LedgerResultKind::kCommitted;
  const bool not_applied = value.result_kind == LedgerResultKind::kNotApplied;
  if (!committed && !not_applied) {
    SetError(error, "ledger result kind is invalid");
    return false;
  }

  const bool has_committed_version =
      value.committed_inode != 0 && value.committed_generation != 0;
  const bool has_current_version =
      value.current_inode != 0 && value.current_generation != 0;
  const bool committed_version_empty =
      value.committed_inode == 0 && value.committed_generation == 0;
  const bool current_version_empty =
      value.current_inode == 0 && value.current_generation == 0;

  if (committed) {
    if (value.result_code != LedgerResultCode::kOk ||
        !has_committed_version || !current_version_empty) {
      SetError(error, "committed ledger result has inconsistent versions");
      return false;
    }
    return true;
  }

  const bool expected_rejection =
      (create && value.result_code == LedgerResultCode::kAlreadyExists) ||
      (replace && value.result_code == LedgerResultCode::kVersionMismatch);
  if (!expected_rejection || !committed_version_empty ||
      !has_current_version) {
    SetError(error, "not-applied ledger result is inconsistent");
    return false;
  }
  return true;
}

std::uint32_t ComputeChecksum(RequestLedgerBytes bytes) {
  PutLe32(bytes.data() + kChecksumOffset, 0);
  return ondisk::Crc32c(bytes.data(), bytes.size());
}

}  // namespace

bool EncodeRequestLedgerRecord(const RequestLedgerRecord& value,
                               RequestLedgerBytes* output,
                               std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (output == nullptr) {
    SetError(error, "ledger output is required");
    return false;
  }
  if (!ValidateRecord(value, error)) {
    return false;
  }

  RequestLedgerBytes encoded{};
  std::copy(kLedgerMagic.begin(), kLedgerMagic.end(), encoded.begin());
  PutLe16(encoded.data() + 4, kRequestLedgerFormatVersion);
  encoded[6] = static_cast<std::uint8_t>(value.operation);
  encoded[7] = static_cast<std::uint8_t>(value.result_kind);
  std::copy(value.request_id.begin(), value.request_id.end(),
            encoded.begin() + 8);
  std::copy(value.fingerprint.begin(), value.fingerprint.end(),
            encoded.begin() + 24);
  PutLe32(encoded.data() + 56,
          static_cast<std::uint32_t>(value.result_code));
  PutLe32(encoded.data() + 60, value.committed_inode);
  PutLe64(encoded.data() + 64, value.committed_generation);
  PutLe32(encoded.data() + 72, value.current_inode);
  PutLe64(encoded.data() + 76, value.current_generation);
  PutLe64(encoded.data() + 84, value.sequence);
  PutLe32(encoded.data() + kChecksumOffset, ComputeChecksum(encoded));
  *output = encoded;
  return true;
}

LedgerDecodeStatus DecodeRequestLedgerRecord(
    const RequestLedgerBytes& input, std::uint64_t expected_sequence,
    RequestLedgerRecord* output, std::string* error) {
  if (error != nullptr) {
    error->clear();
  }
  if (output == nullptr || expected_sequence == 0) {
    SetError(error, "ledger decode arguments are invalid");
    return LedgerDecodeStatus::kCorrupt;
  }
  if (IsAllZero(input)) {
    return LedgerDecodeStatus::kEmpty;
  }
  if (!std::equal(kLedgerMagic.begin(), kLedgerMagic.end(), input.begin()) ||
      GetLe16(input.data() + 4) != kRequestLedgerFormatVersion) {
    SetError(error, "ledger header does not match the v1 format");
    return LedgerDecodeStatus::kCorrupt;
  }
  if (GetLe32(input.data() + kChecksumOffset) != ComputeChecksum(input)) {
    SetError(error, "ledger checksum mismatch");
    return LedgerDecodeStatus::kCorrupt;
  }
  if (!std::all_of(input.begin() + kReservedOffset, input.end(),
                   [](std::uint8_t value) { return value == 0; })) {
    SetError(error, "ledger reserved bytes are not zero");
    return LedgerDecodeStatus::kCorrupt;
  }

  RequestLedgerRecord decoded;
  decoded.operation = static_cast<MutationOperation>(input[6]);
  decoded.result_kind = static_cast<LedgerResultKind>(input[7]);
  std::copy_n(input.begin() + 8, decoded.request_id.size(),
              decoded.request_id.begin());
  std::copy_n(input.begin() + 24, decoded.fingerprint.size(),
              decoded.fingerprint.begin());
  decoded.result_code =
      static_cast<LedgerResultCode>(GetLe32(input.data() + 56));
  decoded.committed_inode = GetLe32(input.data() + 60);
  decoded.committed_generation = GetLe64(input.data() + 64);
  decoded.current_inode = GetLe32(input.data() + 72);
  decoded.current_generation = GetLe64(input.data() + 76);
  decoded.sequence = GetLe64(input.data() + 84);
  if (decoded.sequence != expected_sequence) {
    SetError(error, "ledger sequence does not match its slot");
    return LedgerDecodeStatus::kCorrupt;
  }
  if (!ValidateRecord(decoded, error)) {
    return LedgerDecodeStatus::kCorrupt;
  }

  *output = decoded;
  return LedgerDecodeStatus::kRecord;
}

}  // namespace eufs::object_store
