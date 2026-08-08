#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace eufs::ondisk {

// 以下常量共同定义 EUFS v1 的持久化 ABI；修改任意值都需要提升格式版本。
constexpr std::uint32_t kFormatVersion = 1;
// incompat 位表示不了解该能力的实现禁止读写镜像。
constexpr std::uint64_t kFeatureIncompatRequestLedger = 1ULL << 0U;
constexpr std::uint64_t kSupportedFeatureIncompat =
    kFeatureIncompatRequestLedger;
// 所有 metadata/data/journal 块固定为 4096 字节。
constexpr std::uint32_t kBlockSize = 4096;
// journal 区域前两个块分别保存 A/B control copy。
constexpr std::uint32_t kJournalControlBlockCount = 2;
// 最小 ring 必须至少容纳 descriptor、一个 payload 和 COMMIT。
constexpr std::uint32_t kMinimumJournalRingBlocks = 3;
constexpr std::uint32_t kMinimumJournalBlocks =
    kJournalControlBlockCount + kMinimumJournalRingBlocks;
// superblock 只使用块开头 256 字节，其余字节必须按编码规则清零。
constexpr std::size_t kSuperblockHeaderSize = 256;
// 每个 inode 在 inode table 中固定占 128 字节。
constexpr std::size_t kInodeRecordSize = 128;
// inode 内直接保存 12 个数据块指针。
constexpr std::size_t kDirectBlockCount = 12;
// 目录项固定头包含 inode、record_length、name_length 和 file_type。
constexpr std::size_t kDirectoryEntryHeaderSize = 8;
// 单个目录项名称最长 255 字节。
constexpr std::size_t kMaxNameLength = 255;
// v1 最大文件由 12 个直接块和一个单级间接块共同决定。
constexpr std::uint64_t kMaxFileSize =
    (kDirectBlockCount + kBlockSize / sizeof(std::uint32_t)) *
    static_cast<std::uint64_t>(kBlockSize);

// 一个完整磁盘块的内存表示。
using Block = std::array<std::uint8_t, kBlockSize>;
// 一个固定宽度 inode 记录的编码字节。
using InodeBytes = std::array<std::uint8_t, kInodeRecordSize>;

// 描述镜像中的连续块区域，范围为 [start_block, start_block + block_count)。
struct Region {
  std::uint32_t start_block{0};
  std::uint32_t block_count{0};
};

// superblock 解码后的字段；它描述整张镜像的几何和身份。
struct Superblock {
  // 镜像内可寻址块总数。
  std::uint32_t total_blocks{0};
  // inode bitmap 和 inode table 支持的 inode 总数。
  std::uint32_t total_inodes{0};
  // v1 根目录固定使用 inode 1。
  std::uint32_t root_inode{1};
  // 以下五个区域必须有序、互不重叠且位于镜像边界内。
  Region inode_bitmap;
  Region block_bitmap;
  Region inode_table;
  Region journal;
  Region data;
  // 三类特性位为未来兼容升级保留。
  std::uint64_t feature_compat{0};
  std::uint64_t feature_ro_compat{0};
  std::uint64_t feature_incompat{0};
  // UUID 同时写入日志 descriptor，防止跨镜像误回放。
  std::array<std::uint8_t, 16> filesystem_uuid{};
  // 格式化时的 CLOCK_REALTIME 纳秒时间戳。
  std::uint64_t created_time_ns{0};
};

// inode table 中一个 128 字节记录的语义表示。
struct InodeRecord {
  // 记录自带 inode 编号，解码时必须与表位置互相验证。
  std::uint32_t inode_number{0};
  // POSIX 文件类型位和权限位。
  std::uint32_t mode{0};
  std::uint32_t uid{0};
  std::uint32_t gid{0};
  // 当前目录项/硬链接引用数。
  std::uint32_t link_count{0};
  // 文件有效字节数，决定需要解析多少个块指针。
  std::uint64_t size{0};
  std::uint64_t atime_ns{0};
  std::uint64_t mtime_ns{0};
  std::uint64_t ctime_ns{0};
  // flags 为格式扩展保留；generation 用于区分 inode 生命周期版本。
  std::uint32_t flags{0};
  std::uint64_t generation{0};
  // 前 12 个逻辑块直接映射到物理数据块。
  std::array<std::uint32_t, kDirectBlockCount> direct_blocks{};
  // 超过 12 个逻辑块时，指向保存 uint32_t 块号数组的单级间接块。
  std::uint32_t indirect_block{0};
};

// 目录项冗余保存目标类型，便于 readdir；inode.mode 仍是最终权威。
enum class DirectoryFileType : std::uint8_t {
  kUnknown = 0,
  kRegular = 1,
  kDirectory = 2,
};

// 从目录数据块解码后的可变长目录项。
struct DirectoryEntry {
  std::uint32_t inode{0};
  DirectoryFileType file_type{DirectoryFileType::kUnknown};
  std::string name;
  // record_length 决定下一个目录项位置，必须满足对齐和块边界约束。
  std::uint16_t record_length{0};
};

// 计算 EUFS 格式统一使用的 CRC32C。
std::uint32_t Crc32c(const std::uint8_t* data, std::size_t size);

// 根据总块数、inode 数和 journal 容量计算五个连续区域的合法布局。
bool BuildSuperblockLayout(std::uint32_t total_blocks,
                           std::uint32_t total_inodes,
                           std::uint32_t journal_blocks, Superblock* output,
                           std::string* error);

// 编码/解码函数是磁盘字节与主机结构之间的唯一转换边界。
bool EncodeSuperblock(const Superblock& value, Block* output,
                      std::string* error);
bool DecodeSuperblock(const Block& input, Superblock* output,
                      std::string* error);

bool EncodeInode(const InodeRecord& value, InodeBytes* output,
                 std::string* error);
// expected_inode_number 用来交叉验证记录内容和 inode table 槽位一致。
bool DecodeInode(const InodeBytes& input, std::uint32_t expected_inode_number,
                 InodeRecord* output, std::string* error);

// 计算名称长度对应的最小对齐目录项长度。
std::size_t MinimumDirectoryRecordLength(std::size_t name_length);
// 在调用者指定的 record_length 范围内编码一个目录项并清零填充区。
bool EncodeDirectoryEntry(const DirectoryEntry& value,
                          std::uint16_t record_length, std::uint8_t* output,
                          std::size_t output_size, std::string* error);
// 从当前块剩余字节中严格解码一个目录项。
bool DecodeDirectoryEntry(const std::uint8_t* input, std::size_t input_size,
                          DirectoryEntry* output, std::string* error);

}  // namespace eufs::ondisk
