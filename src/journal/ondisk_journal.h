#pragma once

#include "metadata/ondisk_format.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace eufs::journal {

// 以下常量定义 EUFS v1 journal 的持久化 ABI。
constexpr std::uint32_t kJournalFormatVersion = 1;
// control 有效头部、descriptor/COMMIT 固定头部和单个 descriptor entry 字节数。
constexpr std::size_t kJournalControlHeaderSize = 128;
constexpr std::size_t kJournalRecordHeaderSize = 64;
constexpr std::size_t kDescriptorEntrySize = 16;
// 一个 descriptor 块可容纳的最大 metadata payload 数量。
constexpr std::size_t kMaxDescriptorEntries =
    (ondisk::kBlockSize - kJournalRecordHeaderSize) / kDescriptorEntrySize;

// ring 中有结构语义的记录类型；payload 本身是完整 metadata 块，不带额外头。
enum class RecordType : std::uint32_t {
  kDescriptor = 1,
  kCommit = 2,
};

// 两份 control 的物理身份；更新时永远写非当前 copy。
enum class ControlCopy {
  kA,
  kB,
};

// generation 使用环绕序号比较；差值恰好半圈时无法确定谁更新。
enum class GenerationComparison {
  kEqual,
  kFirstNewer,
  kSecondNewer,
  kAmbiguous,
};

// A/B control 解码后的事务边界状态。
struct JournalControl {
  // ring_blocks 不包含前面的两个 control 块。
  std::uint32_t ring_blocks{0};
  // UUID 必须与 superblock 一致。
  std::array<std::uint8_t, 16> filesystem_uuid{};
  // 每次持久化下一份 control 时递增，用于 A/B 选择。
  std::uint64_t generation{0};
  // tail 指向 descriptor，head 指向事务末尾之后，used_blocks 描述暴露长度。
  std::uint32_t head{0};
  std::uint32_t tail{0};
  std::uint32_t used_blocks{0};
  std::uint32_t state_flags{0};
  // 下一个可分配事务号；当前暴露事务号为 next_transaction_id - 1。
  std::uint64_t next_transaction_id{0};
  std::uint64_t feature_compat{0};
  std::uint64_t feature_ro_compat{0};
  std::uint64_t feature_incompat{0};
  // checksum 覆盖固定 control 编码范围。
  std::uint32_t checksum{0};
};

// descriptor 中一个 metadata after-image 的位置与最终 home block。
struct DescriptorEntry {
  std::uint32_t home_block{0};
  std::uint32_t payload_ring_index{0};
  std::uint32_t payload_crc32c{0};
  std::uint32_t flags{0};
};

// 一个 descriptor 完整描述本事务全部 metadata payload。
struct DescriptorRecord {
  std::uint64_t transaction_id{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::uint32_t transaction_block_count{0};
  std::uint32_t flags{0};
  std::uint32_t checksum{0};
  std::vector<DescriptorEntry> entries;
};

// COMMIT 重新绑定事务号、descriptor 位置/CRC 和事务长度，防止 stale COMMIT 误匹配。
struct CommitRecord {
  std::uint64_t transaction_id{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::uint32_t entry_count{0};
  std::uint32_t transaction_block_count{0};
  std::uint32_t descriptor_ring_index{0};
  std::uint32_t descriptor_crc32c{0};
  std::uint32_t checksum{0};
};

// control 编解码负责 magic、版本、保留位、几何和 CRC 校验。
bool EncodeControl(const JournalControl& value, ondisk::Block* output,
                   std::uint32_t* checksum, std::string* error);
bool DecodeControl(const ondisk::Block& input, JournalControl* output,
                   std::string* error);

// 比较两个可能环绕的 64 位 generation。
GenerationComparison CompareGenerations(std::uint64_t first,
                                        std::uint64_t second);
// 分别解码 A/B，结合 UUID/ring 几何和 generation 选择唯一当前 control。
bool SelectControl(const ondisk::Block& control_a,
                   const ondisk::Block& control_b,
                   const std::array<std::uint8_t, 16>& expected_uuid,
                   std::uint32_t expected_ring_blocks,
                   JournalControl* output, ControlCopy* selected_copy,
                   std::string* error);

// descriptor 与 COMMIT 都编码为一个固定 4096 字节块。
bool EncodeDescriptor(const DescriptorRecord& value, ondisk::Block* output,
                      std::uint32_t* checksum, std::string* error);
bool DecodeDescriptor(const ondisk::Block& input, DescriptorRecord* output,
                      std::string* error);

bool EncodeCommit(const CommitRecord& value, ondisk::Block* output,
                  std::string* error);
bool DecodeCommit(const ondisk::Block& input, CommitRecord* output,
                  std::string* error);

// 验证 COMMIT 是否确实绑定给定 descriptor，而不只是事务号碰巧相同。
bool CommitMatchesDescriptor(const DescriptorRecord& descriptor,
                             std::uint32_t descriptor_ring_index,
                             const CommitRecord& commit,
                             std::string* error);

}  // namespace eufs::journal
