#include "journal/ondisk_journal.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <unordered_set>

namespace eufs::journal {
namespace {

// 三种记录各有独立 8 字节 magic，避免把任意 payload 或旧数据误解码成协议记录。
constexpr std::array<std::uint8_t, 8> kDescriptorMagic = {
    'E', 'U', 'F', 'S', 'J', 'D', 'S', '1'};
constexpr std::array<std::uint8_t, 8> kCommitMagic = {
    'E', 'U', 'F', 'S', 'J', 'C', 'M', '1'};
constexpr std::array<std::uint8_t, 8> kControlMagic = {
    'E', 'U', 'F', 'S', 'J', 'C', 'T', '1'};
constexpr std::size_t kRecordChecksumOffset = 60;
constexpr std::size_t kControlChecksumOffset = 124;

// 写入可选错误文本。
void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
}

// 所有多字节整数显式按 little-endian 编解码，磁盘格式不依赖宿主机端序或对齐。
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

// 检查固定记录有效区之后的保留字节必须全部为 0，防止未知扩展被旧版本静默接受。
bool IsZeroRange(const ondisk::Block& block, std::size_t begin) {
  return std::all_of(block.begin() + begin, block.end(),
                     [](std::uint8_t value) { return value == 0; });
}

// 计算整块 CRC 时先把 checksum 字段清零，编码与解码使用同一规则。
std::uint32_t ComputeBlockChecksum(ondisk::Block block,
                                   std::size_t checksum_offset) {
  PutLe32(block.data() + checksum_offset, 0);
  return ondisk::Crc32c(block.data(), block.size());
}

// 验证 control 的 ring 几何、head/tail/used 关系和协议保留位。
bool ValidateControlFields(const JournalControl& value, std::string* error) {
  if (value.ring_blocks < ondisk::kMinimumJournalRingBlocks ||
      value.head >= value.ring_blocks ||
      value.tail >= value.ring_blocks ||
      value.used_blocks > value.ring_blocks ||
      (value.used_blocks != 0 &&
       value.used_blocks < ondisk::kMinimumJournalRingBlocks) ||
      value.next_transaction_id == 0) {
    SetError(error, "journal control positions or counters are invalid");
    return false;
  }
  const std::uint32_t expected_head = static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(value.tail) + value.used_blocks) %
      value.ring_blocks);
  if (value.head != expected_head) {
    SetError(error, "journal control head, tail, and used count disagree");
    return false;
  }
  if (value.state_flags != 0 || value.feature_compat != 0 ||
      value.feature_ro_compat != 0 || value.feature_incompat != 0) {
    SetError(error, "journal control requires unsupported v1 flags");
    return false;
  }
  return true;
}

// 判断两份 control 是否描述完全相同状态，用于 generation 相同场景。
bool ControlsEquivalent(const JournalControl& first,
                        const JournalControl& second) {
  return first.ring_blocks == second.ring_blocks &&
         first.filesystem_uuid == second.filesystem_uuid &&
         first.generation == second.generation && first.head == second.head &&
         first.tail == second.tail &&
         first.used_blocks == second.used_blocks &&
         first.state_flags == second.state_flags &&
         first.next_transaction_id == second.next_transaction_id &&
         first.feature_compat == second.feature_compat &&
         first.feature_ro_compat == second.feature_ro_compat &&
         first.feature_incompat == second.feature_incompat;
}

// 验证 descriptor entry 数量、home block 唯一性和 flags 保留位。
bool ValidateDescriptorEntries(const std::vector<DescriptorEntry>& entries,
                               std::string* error) {
  if (entries.empty() || entries.size() > kMaxDescriptorEntries) {
    SetError(error, "descriptor entry count is outside the v1 range");
    return false;
  }
  std::unordered_set<std::uint32_t> home_blocks;
  std::unordered_set<std::uint32_t> payload_positions;
  for (const auto& entry : entries) {
    if (entry.home_block == 0 || entry.flags != 0) {
      SetError(error, "descriptor entry has an invalid home block or flags");
      return false;
    }
    if (!home_blocks.insert(entry.home_block).second) {
      SetError(error, "descriptor contains a duplicate home block");
      return false;
    }
    if (!payload_positions.insert(entry.payload_ring_index).second) {
      SetError(error, "descriptor contains a duplicate payload position");
      return false;
    }
  }
  return true;
}

}  // 匿名命名空间：字节编解码辅助函数不导出。

// 把内存 control 严格编码为固定 4096 字节块并写入 CRC。
bool EncodeControl(const JournalControl& value, ondisk::Block* output,
                   std::uint32_t* checksum, std::string* error) {
  if (output == nullptr) {
    SetError(error, "journal control output is required");
    return false;
  }
  if (!ValidateControlFields(value, error)) {
    return false;
  }

  output->fill(0);
  std::copy(kControlMagic.begin(), kControlMagic.end(), output->begin());
  PutLe32(output->data() + 8, kJournalFormatVersion);
  PutLe32(output->data() + 12,
          static_cast<std::uint32_t>(kJournalControlHeaderSize));
  PutLe32(output->data() + 16, ondisk::kBlockSize);
  PutLe32(output->data() + 20, value.ring_blocks);
  std::copy(value.filesystem_uuid.begin(), value.filesystem_uuid.end(),
            output->begin() + 24);
  PutLe64(output->data() + 40, value.generation);
  PutLe32(output->data() + 48, value.head);
  PutLe32(output->data() + 52, value.tail);
  PutLe32(output->data() + 56, value.used_blocks);
  PutLe32(output->data() + 60, value.state_flags);
  PutLe64(output->data() + 64, value.next_transaction_id);
  PutLe64(output->data() + 72, value.feature_compat);
  PutLe64(output->data() + 80, value.feature_ro_compat);
  PutLe64(output->data() + 88, value.feature_incompat);

  const std::uint32_t computed =
      ComputeBlockChecksum(*output, kControlChecksumOffset);
  PutLe32(output->data() + kControlChecksumOffset, computed);
  if (checksum != nullptr) {
    *checksum = computed;
  }
  return true;
}

// 验证 magic、版本、CRC、保留字节和字段不变量后解码 control。
bool DecodeControl(const ondisk::Block& input, JournalControl* output,
                   std::string* error) {
  if (output == nullptr) {
    SetError(error, "journal control output is required");
    return false;
  }
  if (!std::equal(kControlMagic.begin(), kControlMagic.end(), input.begin()) ||
      GetLe32(input.data() + 8) != kJournalFormatVersion ||
      GetLe32(input.data() + 12) != kJournalControlHeaderSize ||
      GetLe32(input.data() + 16) != ondisk::kBlockSize) {
    SetError(error, "journal control header does not match the v1 format");
    return false;
  }
  const std::uint32_t stored_checksum =
      GetLe32(input.data() + kControlChecksumOffset);
  if (stored_checksum !=
      ComputeBlockChecksum(input, kControlChecksumOffset)) {
    SetError(error, "journal control checksum mismatch");
    return false;
  }
  if (!std::all_of(input.begin() + 96, input.begin() + 124,
                   [](std::uint8_t value) { return value == 0; }) ||
      !IsZeroRange(input, kJournalControlHeaderSize)) {
    SetError(error, "journal control reserved bytes are not zero");
    return false;
  }

  JournalControl decoded;
  decoded.ring_blocks = GetLe32(input.data() + 20);
  std::copy_n(input.begin() + 24, decoded.filesystem_uuid.size(),
              decoded.filesystem_uuid.begin());
  decoded.generation = GetLe64(input.data() + 40);
  decoded.head = GetLe32(input.data() + 48);
  decoded.tail = GetLe32(input.data() + 52);
  decoded.used_blocks = GetLe32(input.data() + 56);
  decoded.state_flags = GetLe32(input.data() + 60);
  decoded.next_transaction_id = GetLe64(input.data() + 64);
  decoded.feature_compat = GetLe64(input.data() + 72);
  decoded.feature_ro_compat = GetLe64(input.data() + 80);
  decoded.feature_incompat = GetLe64(input.data() + 88);
  decoded.checksum = stored_checksum;
  if (!ValidateControlFields(decoded, error)) {
    return false;
  }
  *output = decoded;
  return true;
}

// 使用模 2^64 序号规则比较 generation，半圈差值返回 ambiguous。
GenerationComparison CompareGenerations(std::uint64_t first,
                                        std::uint64_t second) {
  if (first == second) {
    return GenerationComparison::kEqual;
  }
  constexpr std::uint64_t kHalfRange = std::uint64_t{1} << 63U;
  const std::uint64_t difference = first - second;
  if (difference == kHalfRange) {
    return GenerationComparison::kAmbiguous;
  }
  return difference < kHalfRange ? GenerationComparison::kFirstNewer
                                 : GenerationComparison::kSecondNewer;
}

// 独立解码 A/B control，排除无效 copy，并选择唯一更新者或等价副本。
bool SelectControl(const ondisk::Block& control_a,
                   const ondisk::Block& control_b,
                   const std::array<std::uint8_t, 16>& expected_uuid,
                   std::uint32_t expected_ring_blocks,
                   JournalControl* output, ControlCopy* selected_copy,
                   std::string* error) {
  if (output == nullptr || selected_copy == nullptr ||
      expected_ring_blocks < ondisk::kMinimumJournalRingBlocks) {
    SetError(error, "journal control selection arguments are invalid");
    return false;
  }

  JournalControl decoded_a;
  JournalControl decoded_b;
  std::string ignored_error;
  bool valid_a = DecodeControl(control_a, &decoded_a, &ignored_error);
  bool valid_b = DecodeControl(control_b, &decoded_b, &ignored_error);
  valid_a = valid_a && decoded_a.filesystem_uuid == expected_uuid &&
            decoded_a.ring_blocks == expected_ring_blocks;
  valid_b = valid_b && decoded_b.filesystem_uuid == expected_uuid &&
            decoded_b.ring_blocks == expected_ring_blocks;

  if (!valid_a && !valid_b) {
    SetError(error, "both journal control copies are invalid");
    return false;
  }
  if (valid_a && !valid_b) {
    *output = decoded_a;
    *selected_copy = ControlCopy::kA;
    return true;
  }
  if (!valid_a && valid_b) {
    *output = decoded_b;
    *selected_copy = ControlCopy::kB;
    return true;
  }

  const auto comparison =
      CompareGenerations(decoded_a.generation, decoded_b.generation);
  if (comparison == GenerationComparison::kAmbiguous) {
    SetError(error, "journal control generations are half-range ambiguous");
    return false;
  }
  if (comparison == GenerationComparison::kEqual) {
    if (!ControlsEquivalent(decoded_a, decoded_b)) {
      SetError(error, "equal journal generations contain different states");
      return false;
    }
    *output = decoded_a;
    *selected_copy = ControlCopy::kA;
    return true;
  }
  if (comparison == GenerationComparison::kFirstNewer) {
    *output = decoded_a;
    *selected_copy = ControlCopy::kA;
  } else {
    *output = decoded_b;
    *selected_copy = ControlCopy::kB;
  }
  return true;
}

// 编码 descriptor 头和全部 entry；checksum 同时返回给 COMMIT 绑定。
bool EncodeDescriptor(const DescriptorRecord& value, ondisk::Block* output,
                      std::uint32_t* checksum, std::string* error) {
  if (output == nullptr || value.transaction_id == 0 || value.flags != 0 ||
      !ValidateDescriptorEntries(value.entries, error)) {
    if (output == nullptr) {
      SetError(error, "descriptor output is required");
    }
    return false;
  }
  const auto expected_blocks =
      static_cast<std::uint32_t>(value.entries.size() + 2U);
  if (value.transaction_block_count != expected_blocks) {
    SetError(error, "descriptor transaction block count is inconsistent");
    return false;
  }

  output->fill(0);
  std::copy(kDescriptorMagic.begin(), kDescriptorMagic.end(), output->begin());
  PutLe32(output->data() + 8, kJournalFormatVersion);
  PutLe32(output->data() + 12,
          static_cast<std::uint32_t>(kJournalRecordHeaderSize));
  PutLe32(output->data() + 16,
          static_cast<std::uint32_t>(RecordType::kDescriptor));
  PutLe32(output->data() + 20,
          static_cast<std::uint32_t>(value.entries.size()));
  PutLe64(output->data() + 24, value.transaction_id);
  std::copy(value.filesystem_uuid.begin(), value.filesystem_uuid.end(),
            output->begin() + 32);
  PutLe32(output->data() + 48, value.transaction_block_count);
  PutLe32(output->data() + 52,
          static_cast<std::uint32_t>(kJournalRecordHeaderSize));
  PutLe32(output->data() + 56, value.flags);

  for (std::size_t index = 0; index < value.entries.size(); ++index) {
    const auto& entry = value.entries[index];
    std::uint8_t* encoded = output->data() + kJournalRecordHeaderSize +
                            index * kDescriptorEntrySize;
    PutLe32(encoded, entry.home_block);
    PutLe32(encoded + 4, entry.payload_ring_index);
    PutLe32(encoded + 8, entry.payload_crc32c);
    PutLe32(encoded + 12, entry.flags);
  }

  const std::uint32_t computed =
      ComputeBlockChecksum(*output, kRecordChecksumOffset);
  PutLe32(output->data() + kRecordChecksumOffset, computed);
  if (checksum != nullptr) {
    *checksum = computed;
  }
  return true;
}

// 解码并验证 descriptor，不接受超量 entry、非零保留字段或 CRC 错误。
bool DecodeDescriptor(const ondisk::Block& input, DescriptorRecord* output,
                      std::string* error) {
  if (output == nullptr) {
    SetError(error, "descriptor output is required");
    return false;
  }
  if (!std::equal(kDescriptorMagic.begin(), kDescriptorMagic.end(),
                  input.begin()) ||
      GetLe32(input.data() + 8) != kJournalFormatVersion ||
      GetLe32(input.data() + 12) != kJournalRecordHeaderSize ||
      GetLe32(input.data() + 16) !=
          static_cast<std::uint32_t>(RecordType::kDescriptor) ||
      GetLe32(input.data() + 52) != kJournalRecordHeaderSize) {
    SetError(error, "descriptor header does not match the v1 format");
    return false;
  }
  const std::uint32_t stored_checksum =
      GetLe32(input.data() + kRecordChecksumOffset);
  if (stored_checksum !=
      ComputeBlockChecksum(input, kRecordChecksumOffset)) {
    SetError(error, "descriptor checksum mismatch");
    return false;
  }

  DescriptorRecord decoded;
  const std::uint32_t entry_count = GetLe32(input.data() + 20);
  if (entry_count == 0 || entry_count > kMaxDescriptorEntries) {
    SetError(error, "descriptor entry count is outside the v1 range");
    return false;
  }
  decoded.transaction_id = GetLe64(input.data() + 24);
  if (decoded.transaction_id == 0) {
    SetError(error, "descriptor transaction id is invalid");
    return false;
  }
  std::copy_n(input.begin() + 32, decoded.filesystem_uuid.size(),
              decoded.filesystem_uuid.begin());
  decoded.transaction_block_count = GetLe32(input.data() + 48);
  decoded.flags = GetLe32(input.data() + 56);
  decoded.checksum = stored_checksum;
  if (decoded.flags != 0 ||
      decoded.transaction_block_count != entry_count + 2U) {
    SetError(error, "descriptor flags or block count are invalid");
    return false;
  }

  decoded.entries.reserve(entry_count);
  for (std::uint32_t index = 0; index < entry_count; ++index) {
    const std::uint8_t* encoded =
        input.data() + kJournalRecordHeaderSize +
        static_cast<std::size_t>(index) * kDescriptorEntrySize;
    decoded.entries.push_back(DescriptorEntry{
        GetLe32(encoded), GetLe32(encoded + 4), GetLe32(encoded + 8),
        GetLe32(encoded + 12)});
  }
  const std::size_t used =
      kJournalRecordHeaderSize + entry_count * kDescriptorEntrySize;
  if (!IsZeroRange(input, used) ||
      !ValidateDescriptorEntries(decoded.entries, error)) {
    if (!IsZeroRange(input, used)) {
      SetError(error, "descriptor reserved bytes are not zero");
    }
    return false;
  }
  *output = std::move(decoded);
  return true;
}

// 编码固定宽度 COMMIT，它不携带 payload，只绑定 descriptor 身份和边界。
bool EncodeCommit(const CommitRecord& value, ondisk::Block* output,
                  std::string* error) {
  if (output == nullptr || value.transaction_id == 0 || value.entry_count == 0 ||
      value.entry_count > kMaxDescriptorEntries ||
      value.transaction_block_count != value.entry_count + 2U) {
    SetError(error, "commit fields do not describe a valid v1 transaction");
    return false;
  }

  output->fill(0);
  std::copy(kCommitMagic.begin(), kCommitMagic.end(), output->begin());
  PutLe32(output->data() + 8, kJournalFormatVersion);
  PutLe32(output->data() + 12,
          static_cast<std::uint32_t>(kJournalRecordHeaderSize));
  PutLe32(output->data() + 16,
          static_cast<std::uint32_t>(RecordType::kCommit));
  PutLe32(output->data() + 20, value.entry_count);
  PutLe64(output->data() + 24, value.transaction_id);
  std::copy(value.filesystem_uuid.begin(), value.filesystem_uuid.end(),
            output->begin() + 32);
  PutLe32(output->data() + 48, value.transaction_block_count);
  PutLe32(output->data() + 52, value.descriptor_ring_index);
  PutLe32(output->data() + 56, value.descriptor_crc32c);
  const std::uint32_t computed =
      ComputeBlockChecksum(*output, kRecordChecksumOffset);
  PutLe32(output->data() + kRecordChecksumOffset, computed);
  return true;
}

// 解码并验证 COMMIT 的 magic、版本、保留位和整块 CRC。
bool DecodeCommit(const ondisk::Block& input, CommitRecord* output,
                  std::string* error) {
  if (output == nullptr) {
    SetError(error, "commit output is required");
    return false;
  }
  if (!std::equal(kCommitMagic.begin(), kCommitMagic.end(), input.begin()) ||
      GetLe32(input.data() + 8) != kJournalFormatVersion ||
      GetLe32(input.data() + 12) != kJournalRecordHeaderSize ||
      GetLe32(input.data() + 16) !=
          static_cast<std::uint32_t>(RecordType::kCommit)) {
    SetError(error, "commit header does not match the v1 format");
    return false;
  }
  const std::uint32_t stored_checksum =
      GetLe32(input.data() + kRecordChecksumOffset);
  if (stored_checksum !=
      ComputeBlockChecksum(input, kRecordChecksumOffset)) {
    SetError(error, "commit checksum mismatch");
    return false;
  }
  if (!IsZeroRange(input, kJournalRecordHeaderSize)) {
    SetError(error, "commit reserved bytes are not zero");
    return false;
  }

  CommitRecord decoded;
  decoded.entry_count = GetLe32(input.data() + 20);
  decoded.transaction_id = GetLe64(input.data() + 24);
  std::copy_n(input.begin() + 32, decoded.filesystem_uuid.size(),
              decoded.filesystem_uuid.begin());
  decoded.transaction_block_count = GetLe32(input.data() + 48);
  decoded.descriptor_ring_index = GetLe32(input.data() + 52);
  decoded.descriptor_crc32c = GetLe32(input.data() + 56);
  decoded.checksum = stored_checksum;
  if (decoded.transaction_id == 0 || decoded.entry_count == 0 ||
      decoded.entry_count > kMaxDescriptorEntries ||
      decoded.transaction_block_count != decoded.entry_count + 2U) {
    SetError(error, "commit fields do not describe a valid v1 transaction");
    return false;
  }
  *output = decoded;
  return true;
}

// 交叉检查 COMMIT 的事务号、UUID、entry 数、总长度、descriptor 位置与 CRC。
bool CommitMatchesDescriptor(const DescriptorRecord& descriptor,
                             std::uint32_t descriptor_ring_index,
                             const CommitRecord& commit,
                             std::string* error) {
  if (descriptor.transaction_id != commit.transaction_id ||
      descriptor.filesystem_uuid != commit.filesystem_uuid ||
      descriptor.entries.size() != commit.entry_count ||
      descriptor.transaction_block_count != commit.transaction_block_count ||
      descriptor_ring_index != commit.descriptor_ring_index ||
      descriptor.checksum != commit.descriptor_crc32c) {
    SetError(error, "commit does not bind to the decoded descriptor");
    return false;
  }
  return true;
}

}  // namespace eufs::journal
