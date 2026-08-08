#include "metadata/ondisk_format.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace eufs::ondisk {
namespace {

// EUFS superblock magic 和两个 CRC 字段固定偏移属于 v1 持久化 ABI。
constexpr std::array<std::uint8_t, 8> kMagic{
    'E', 'U', 'F', 'S', 'I', 'M', 'G', '1'};

constexpr std::size_t kSuperblockChecksumOffset = 128;
constexpr std::size_t kInodeChecksumOffset = 124;

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
}

// 16/32/64 位整数均显式 little-endian 编解码，禁止直接 memcpy 主机结构。
void PutLe16(std::uint8_t* output, std::uint16_t value) {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8U);
}

void PutLe32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void PutLe64(std::uint8_t* output, std::uint64_t value) {
  for (std::size_t index = 0; index < 8; ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

std::uint16_t GetLe16(const std::uint8_t* input) {
  return static_cast<std::uint16_t>(input[0]) |
         (static_cast<std::uint16_t>(input[1]) << 8U);
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t GetLe64(const std::uint8_t* input) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(input[index]) << (index * 8U);
  }
  return value;
}

// 向上整除，用于把字节/记录数量转换成块数量。
std::uint32_t CeilDivide(std::uint64_t numerator,
                         std::uint32_t denominator) {
  return static_cast<std::uint32_t>((numerator + denominator - 1U) /
                                    denominator);
}

// 验证连续区域恰好在 expected_end 结束，防止间隙和重叠。
bool RegionEndsAt(const Region& region, std::uint32_t expected_end) {
  return static_cast<std::uint64_t>(region.start_block) +
             region.block_count ==
         expected_end;
}

// 保留区域必须全零，旧 reader 才能拒绝未知新格式扩展。
bool BytesAreZero(const std::uint8_t* bytes, std::size_t size) {
  return std::all_of(bytes, bytes + size,
                     [](std::uint8_t value) { return value == 0; });
}

// 验证 superblock 的总量、区域连续性、最小 journal 和数据区边界。
bool ValidateSuperblock(const Superblock& value, std::string* error) {
  if (value.total_blocks < 8) {
    SetError(error, "total_blocks is too small");
    return false;
  }
  if (value.total_inodes == 0 || value.root_inode == 0 ||
      value.root_inode > value.total_inodes) {
    SetError(error, "root inode is outside the inode range");
    return false;
  }

  const std::uint32_t required_inode_bitmap =
      CeilDivide(value.total_inodes, kBlockSize * 8U);
  const std::uint32_t required_block_bitmap =
      CeilDivide(value.total_blocks, kBlockSize * 8U);
  const std::uint32_t required_inode_table = CeilDivide(
      static_cast<std::uint64_t>(value.total_inodes) * kInodeRecordSize,
      kBlockSize);

  if (value.inode_bitmap.start_block != 1 ||
      value.inode_bitmap.block_count != required_inode_bitmap) {
    SetError(error, "inode bitmap does not match the v1 layout formula");
    return false;
  }
  if (value.block_bitmap.start_block !=
          value.inode_bitmap.start_block +
              value.inode_bitmap.block_count ||
      value.block_bitmap.block_count != required_block_bitmap) {
    SetError(error, "block bitmap does not match the v1 layout formula");
    return false;
  }
  if (value.inode_table.start_block !=
          value.block_bitmap.start_block + value.block_bitmap.block_count ||
      value.inode_table.block_count != required_inode_table) {
    SetError(error, "inode table does not match the v1 layout formula");
    return false;
  }
  const std::uint64_t journal_start =
      static_cast<std::uint64_t>(value.inode_table.start_block) +
      value.inode_table.block_count;
  if (journal_start > std::numeric_limits<std::uint32_t>::max() ||
      value.journal.start_block != journal_start ||
      value.journal.block_count < kMinimumJournalBlocks) {
    SetError(error,
             "journal must follow the inode table and reserve two controls "
             "plus at least three ring blocks");
    return false;
  }
  const std::uint64_t data_start =
      static_cast<std::uint64_t>(value.journal.start_block) +
      value.journal.block_count;
  if (data_start > std::numeric_limits<std::uint32_t>::max() ||
      value.data.start_block != data_start || value.data.block_count == 0 ||
      value.data.start_block >= value.total_blocks ||
      !RegionEndsAt(value.data, value.total_blocks)) {
    SetError(error, "data region does not consume the remaining image blocks");
    return false;
  }
  return true;
}

// v1 inode 只接受普通文件或目录类型。
bool IsKnownMode(std::uint32_t mode) {
  const mode_t type = static_cast<mode_t>(mode) & S_IFMT;
  return type == S_IFREG || type == S_IFDIR;
}

bool IsKnownDirectoryFileType(DirectoryFileType type) {
  return type == DirectoryFileType::kRegular ||
         type == DirectoryFileType::kDirectory;
}

// 目录名称不能含 NUL/斜杠，也不能是空、`.` 或 `..`。
bool IsValidName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.size() <= kMaxNameLength &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

}  // 匿名命名空间：固定宽度字节布局辅助函数不导出。

// Castagnoli 多项式 CRC32C，用于 superblock、inode、journal 和 payload 校验。
std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    crc ^= data[index];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0x82F63B78U & mask);
    }
  }
  return ~crc;
}

// 按 superblock -> inode bitmap -> block bitmap -> inode table -> journal -> data 排布区域。
bool BuildSuperblockLayout(std::uint32_t total_blocks,
                           std::uint32_t total_inodes,
                           std::uint32_t journal_blocks, Superblock* output,
                           std::string* error) {
  if (output == nullptr) {
    SetError(error, "superblock output is null");
    return false;
  }
  if (total_inodes == 0 || journal_blocks < kMinimumJournalBlocks) {
    SetError(error, "inode count or journal size is invalid");
    return false;
  }

  Superblock value;
  value.total_blocks = total_blocks;
  value.total_inodes = total_inodes;
  value.root_inode = 1;
  value.inode_bitmap =
      {1, CeilDivide(total_inodes, kBlockSize * 8U)};
  value.block_bitmap =
      {value.inode_bitmap.start_block + value.inode_bitmap.block_count,
       CeilDivide(total_blocks, kBlockSize * 8U)};
  value.inode_table = {
      value.block_bitmap.start_block + value.block_bitmap.block_count,
      CeilDivide(static_cast<std::uint64_t>(total_inodes) * kInodeRecordSize,
                 kBlockSize)};

  const std::uint64_t journal_start =
      static_cast<std::uint64_t>(value.inode_table.start_block) +
      value.inode_table.block_count;
  const std::uint64_t data_start = journal_start + journal_blocks;
  if (journal_start > std::numeric_limits<std::uint32_t>::max() ||
      data_start >= total_blocks) {
    SetError(error, "image is too small for the requested metadata layout");
    return false;
  }
  value.journal = {static_cast<std::uint32_t>(journal_start), journal_blocks};
  value.data = {static_cast<std::uint32_t>(data_start),
                total_blocks - static_cast<std::uint32_t>(data_start)};
  if (!ValidateSuperblock(value, error)) {
    return false;
  }
  *output = value;
  return true;
}

// 严格验证后编码 superblock，checksum 字段清零参与计算再写回。
bool EncodeSuperblock(const Superblock& value, Block* output,
                      std::string* error) {
  if (output == nullptr) {
    SetError(error, "superblock output is null");
    return false;
  }
  if (!ValidateSuperblock(value, error)) {
    return false;
  }

  output->fill(0);
  std::copy(kMagic.begin(), kMagic.end(), output->begin());
  PutLe32(output->data() + 8, kFormatVersion);
  PutLe32(output->data() + 12, kSuperblockHeaderSize);
  PutLe32(output->data() + 16, kBlockSize);
  PutLe32(output->data() + 20, kInodeRecordSize);
  PutLe32(output->data() + 24, value.total_blocks);
  PutLe32(output->data() + 28, value.total_inodes);
  PutLe32(output->data() + 32, value.root_inode);
  PutLe32(output->data() + 40, value.inode_bitmap.start_block);
  PutLe32(output->data() + 44, value.inode_bitmap.block_count);
  PutLe32(output->data() + 48, value.block_bitmap.start_block);
  PutLe32(output->data() + 52, value.block_bitmap.block_count);
  PutLe32(output->data() + 56, value.inode_table.start_block);
  PutLe32(output->data() + 60, value.inode_table.block_count);
  PutLe32(output->data() + 64, value.journal.start_block);
  PutLe32(output->data() + 68, value.journal.block_count);
  PutLe32(output->data() + 72, value.data.start_block);
  PutLe32(output->data() + 76, value.data.block_count);
  PutLe64(output->data() + 80, value.feature_compat);
  PutLe64(output->data() + 88, value.feature_ro_compat);
  PutLe64(output->data() + 96, value.feature_incompat);
  std::copy(value.filesystem_uuid.begin(), value.filesystem_uuid.end(),
            output->begin() + 104);
  PutLe64(output->data() + 120, value.created_time_ns);
  PutLe32(output->data() + 132, kDirectBlockCount);
  PutLe32(output->data() + 136, 1);
  PutLe32(output->data() + 140, kMaxNameLength);
  PutLe32(output->data() + kSuperblockChecksumOffset,
          Crc32c(output->data(), kSuperblockHeaderSize));
  return true;
}

// 校验 magic/version/CRC/保留字节后解码并再次验证几何。
bool DecodeSuperblock(const Block& input, Superblock* output,
                      std::string* error) {
  if (output == nullptr) {
    SetError(error, "superblock output is null");
    return false;
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), input.begin())) {
    SetError(error, "superblock magic mismatch");
    return false;
  }
  if (GetLe32(input.data() + 8) != kFormatVersion ||
      GetLe32(input.data() + 12) != kSuperblockHeaderSize ||
      GetLe32(input.data() + 16) != kBlockSize ||
      GetLe32(input.data() + 20) != kInodeRecordSize ||
      GetLe32(input.data() + 132) != kDirectBlockCount ||
      GetLe32(input.data() + 136) != 1 ||
      GetLe32(input.data() + 140) != kMaxNameLength) {
    SetError(error, "unsupported superblock format parameters");
    return false;
  }
  if (!BytesAreZero(input.data() + 36, 4) ||
      !BytesAreZero(input.data() + 144, kSuperblockHeaderSize - 144) ||
      !BytesAreZero(input.data() + kSuperblockHeaderSize,
                    input.size() - kSuperblockHeaderSize)) {
    SetError(error, "superblock reserved bytes are not zero");
    return false;
  }

  Block checksum_input = input;
  const std::uint32_t stored_checksum =
      GetLe32(input.data() + kSuperblockChecksumOffset);
  PutLe32(checksum_input.data() + kSuperblockChecksumOffset, 0);
  if (Crc32c(checksum_input.data(), kSuperblockHeaderSize) !=
      stored_checksum) {
    SetError(error, "superblock checksum mismatch");
    return false;
  }

  output->total_blocks = GetLe32(input.data() + 24);
  output->total_inodes = GetLe32(input.data() + 28);
  output->root_inode = GetLe32(input.data() + 32);
  output->inode_bitmap =
      {GetLe32(input.data() + 40), GetLe32(input.data() + 44)};
  output->block_bitmap =
      {GetLe32(input.data() + 48), GetLe32(input.data() + 52)};
  output->inode_table =
      {GetLe32(input.data() + 56), GetLe32(input.data() + 60)};
  output->journal =
      {GetLe32(input.data() + 64), GetLe32(input.data() + 68)};
  output->data =
      {GetLe32(input.data() + 72), GetLe32(input.data() + 76)};
  output->feature_compat = GetLe64(input.data() + 80);
  output->feature_ro_compat = GetLe64(input.data() + 88);
  output->feature_incompat = GetLe64(input.data() + 96);
  std::copy_n(input.begin() + 104, output->filesystem_uuid.size(),
              output->filesystem_uuid.begin());
  output->created_time_ns = GetLe64(input.data() + 120);
  return ValidateSuperblock(*output, error);
}

// 编码固定 128 字节 inode；未使用字段和尾部保持零。
bool EncodeInode(const InodeRecord& value, InodeBytes* output,
                 std::string* error) {
  if (output == nullptr) {
    SetError(error, "inode output is null");
    return false;
  }
  if (value.inode_number == 0 || !IsKnownMode(value.mode)) {
    SetError(error, "inode mode has an unsupported file type");
    return false;
  }
  if (value.size > kMaxFileSize) {
    SetError(error, "inode size exceeds the v1 direct/indirect limit");
    return false;
  }

  output->fill(0);
  PutLe32(output->data() + 0, value.mode);
  PutLe32(output->data() + 4, value.uid);
  PutLe32(output->data() + 8, value.gid);
  PutLe32(output->data() + 12, value.link_count);
  PutLe64(output->data() + 16, value.size);
  PutLe64(output->data() + 24, value.atime_ns);
  PutLe64(output->data() + 32, value.mtime_ns);
  PutLe64(output->data() + 40, value.ctime_ns);
  PutLe32(output->data() + 48, value.flags);
  PutLe32(output->data() + 52, value.inode_number);
  PutLe64(output->data() + 56, value.generation);
  for (std::size_t index = 0; index < value.direct_blocks.size(); ++index) {
    PutLe32(output->data() + 64 + index * sizeof(std::uint32_t),
            value.direct_blocks[index]);
  }
  PutLe32(output->data() + 112, value.indirect_block);
  PutLe32(output->data() + kInodeChecksumOffset,
          Crc32c(output->data(), output->size()));
  return true;
}

// 解码 inode 并验证槽位编号、类型、size 与 direct/indirect 指针形态。
bool DecodeInode(const InodeBytes& input, std::uint32_t expected_inode_number,
                 InodeRecord* output, std::string* error) {
  if (output == nullptr) {
    SetError(error, "inode output is null");
    return false;
  }
  InodeBytes checksum_input = input;
  const std::uint32_t stored_checksum =
      GetLe32(input.data() + kInodeChecksumOffset);
  PutLe32(checksum_input.data() + kInodeChecksumOffset, 0);
  if (Crc32c(checksum_input.data(), checksum_input.size()) !=
      stored_checksum) {
    SetError(error, "inode checksum mismatch");
    return false;
  }
  if (!BytesAreZero(input.data() + 116, 8)) {
    SetError(error, "inode reserved bytes are not zero");
    return false;
  }

  output->mode = GetLe32(input.data() + 0);
  output->uid = GetLe32(input.data() + 4);
  output->gid = GetLe32(input.data() + 8);
  output->link_count = GetLe32(input.data() + 12);
  output->size = GetLe64(input.data() + 16);
  output->atime_ns = GetLe64(input.data() + 24);
  output->mtime_ns = GetLe64(input.data() + 32);
  output->ctime_ns = GetLe64(input.data() + 40);
  output->flags = GetLe32(input.data() + 48);
  output->inode_number = GetLe32(input.data() + 52);
  output->generation = GetLe64(input.data() + 56);
  for (std::size_t index = 0; index < output->direct_blocks.size(); ++index) {
    output->direct_blocks[index] =
        GetLe32(input.data() + 64 + index * sizeof(std::uint32_t));
  }
  output->indirect_block = GetLe32(input.data() + 112);

  if (expected_inode_number == 0 ||
      output->inode_number != expected_inode_number) {
    SetError(error, "inode number does not match its table slot");
    return false;
  }
  if (!IsKnownMode(output->mode)) {
    SetError(error, "inode mode has an unsupported file type");
    return false;
  }
  if (output->size > kMaxFileSize) {
    SetError(error, "inode size exceeds the v1 direct/indirect limit");
    return false;
  }
  return true;
}

// 目录项长度按 4 字节向上对齐。
std::size_t MinimumDirectoryRecordLength(std::size_t name_length) {
  const std::size_t unaligned = kDirectoryEntryHeaderSize + name_length;
  return (unaligned + 3U) & ~std::size_t{3U};
}

// 在显式 record_length 内编码目录项，剩余 slack 清零供后续分裂。
bool EncodeDirectoryEntry(const DirectoryEntry& value,
                          std::uint16_t record_length, std::uint8_t* output,
                          std::size_t output_size, std::string* error) {
  if (output == nullptr) {
    SetError(error, "directory entry output is null");
    return false;
  }
  if (value.inode == 0 || !IsKnownDirectoryFileType(value.file_type) ||
      !IsValidName(value.name)) {
    SetError(error, "directory entry fields are invalid");
    return false;
  }
  const std::size_t minimum = MinimumDirectoryRecordLength(value.name.size());
  if (record_length < minimum || record_length > output_size ||
      record_length > kBlockSize || record_length % 4U != 0) {
    SetError(error, "directory record length is invalid");
    return false;
  }

  std::fill_n(output, record_length, 0);
  PutLe32(output + 0, value.inode);
  PutLe16(output + 4, record_length);
  output[6] = static_cast<std::uint8_t>(value.name.size());
  output[7] = static_cast<std::uint8_t>(value.file_type);
  std::copy(value.name.begin(), value.name.end(), output + 8);
  return true;
}

// 严格检查 record_length、name_length、对齐、类型和名称后解码。
bool DecodeDirectoryEntry(const std::uint8_t* input, std::size_t input_size,
                          DirectoryEntry* output, std::string* error) {
  if (input == nullptr || output == nullptr ||
      input_size < kDirectoryEntryHeaderSize) {
    SetError(error, "directory entry input is too small");
    return false;
  }
  const std::uint16_t record_length = GetLe16(input + 4);
  const std::uint8_t name_length = input[6];
  const auto file_type = static_cast<DirectoryFileType>(input[7]);
  if (record_length < kDirectoryEntryHeaderSize ||
      record_length > input_size || record_length > kBlockSize ||
      record_length % 4U != 0 ||
      name_length > record_length - kDirectoryEntryHeaderSize) {
    SetError(error, "directory record boundaries are invalid");
    return false;
  }
  const std::uint32_t inode = GetLe32(input + 0);
  if (inode == 0) {
    if (name_length != 0 || file_type != DirectoryFileType::kUnknown) {
      SetError(error, "free directory record contains active fields");
      return false;
    }
    if (!BytesAreZero(input + kDirectoryEntryHeaderSize,
                      record_length - kDirectoryEntryHeaderSize)) {
      SetError(error, "free directory record contains non-zero payload");
      return false;
    }
    *output = {0, DirectoryFileType::kUnknown, {}, record_length};
    return true;
  }

  const std::string_view name(reinterpret_cast<const char*>(input + 8),
                              name_length);
  if (!IsKnownDirectoryFileType(file_type) || !IsValidName(name)) {
    SetError(error, "active directory record contains invalid fields");
    return false;
  }
  const std::size_t name_end = kDirectoryEntryHeaderSize + name_length;
  if (!BytesAreZero(input + name_end, record_length - name_end)) {
    SetError(error, "directory record padding is not zero");
    return false;
  }
  output->inode = inode;
  output->file_type = file_type;
  output->name.assign(name);
  output->record_length = record_length;
  return true;
}

}  // namespace eufs::ondisk
