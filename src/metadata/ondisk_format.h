#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace eufs::ondisk {

constexpr std::uint32_t kFormatVersion = 1;
constexpr std::uint32_t kBlockSize = 4096;
constexpr std::uint32_t kJournalControlBlockCount = 2;
constexpr std::uint32_t kMinimumJournalRingBlocks = 3;
constexpr std::uint32_t kMinimumJournalBlocks =
    kJournalControlBlockCount + kMinimumJournalRingBlocks;
constexpr std::size_t kSuperblockHeaderSize = 256;
constexpr std::size_t kInodeRecordSize = 128;
constexpr std::size_t kDirectBlockCount = 12;
constexpr std::size_t kDirectoryEntryHeaderSize = 8;
constexpr std::size_t kMaxNameLength = 255;
constexpr std::uint64_t kMaxFileSize =
    (kDirectBlockCount + kBlockSize / sizeof(std::uint32_t)) *
    static_cast<std::uint64_t>(kBlockSize);

using Block = std::array<std::uint8_t, kBlockSize>;
using InodeBytes = std::array<std::uint8_t, kInodeRecordSize>;

struct Region {
  std::uint32_t start_block{0};
  std::uint32_t block_count{0};
};

struct Superblock {
  std::uint32_t total_blocks{0};
  std::uint32_t total_inodes{0};
  std::uint32_t root_inode{1};
  Region inode_bitmap;
  Region block_bitmap;
  Region inode_table;
  Region journal;
  Region data;
  std::uint64_t feature_compat{0};
  std::uint64_t feature_ro_compat{0};
  std::uint64_t feature_incompat{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::uint64_t created_time_ns{0};
};

struct InodeRecord {
  std::uint32_t inode_number{0};
  std::uint32_t mode{0};
  std::uint32_t uid{0};
  std::uint32_t gid{0};
  std::uint32_t link_count{0};
  std::uint64_t size{0};
  std::uint64_t atime_ns{0};
  std::uint64_t mtime_ns{0};
  std::uint64_t ctime_ns{0};
  std::uint32_t flags{0};
  std::uint64_t generation{0};
  std::array<std::uint32_t, kDirectBlockCount> direct_blocks{};
  std::uint32_t indirect_block{0};
};

enum class DirectoryFileType : std::uint8_t {
  kUnknown = 0,
  kRegular = 1,
  kDirectory = 2,
};

struct DirectoryEntry {
  std::uint32_t inode{0};
  DirectoryFileType file_type{DirectoryFileType::kUnknown};
  std::string name;
  std::uint16_t record_length{0};
};

std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size);

bool BuildSuperblockLayout(std::uint32_t total_blocks,
                           std::uint32_t total_inodes,
                           std::uint32_t journal_blocks, Superblock* output,
                           std::string* error);

bool EncodeSuperblock(const Superblock& value, Block* output,
                      std::string* error);
bool DecodeSuperblock(const Block& input, Superblock* output,
                      std::string* error);

bool EncodeInode(const InodeRecord& value, InodeBytes* output,
                 std::string* error);
bool DecodeInode(const InodeBytes& input, std::uint32_t expected_inode_number,
                 InodeRecord* output, std::string* error);

std::size_t MinimumDirectoryRecordLength(std::size_t name_length);
bool EncodeDirectoryEntry(const DirectoryEntry& value,
                          std::uint16_t record_length, std::uint8_t* output,
                          std::size_t output_size, std::string* error);
bool DecodeDirectoryEntry(const std::uint8_t* input, std::size_t input_size,
                          DirectoryEntry* output, std::string* error);

}  // namespace eufs::ondisk
