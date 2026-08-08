#include "storage/mkfs.h"

#include "journal/ondisk_journal.h"
#include "object/request_ledger_format.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace eufs::storage {
namespace {

// 简单 RAII fd 包装，保证 FormatImage 任意提前返回都会关闭镜像。
class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return value_; }

 private:
  int value_;
};

// 新建镜像失败时自动 unlink 部分文件；force 覆盖既有文件时不能删除用户原路径。
class PartialImageCleanup {
 public:
  PartialImageCleanup(const std::string& path, bool active)
      : path_(path), active_(active) {}
  ~PartialImageCleanup() {
    if (active_) {
      unlink(path_.c_str());
    }
  }

  PartialImageCleanup(const PartialImageCleanup&) = delete;
  PartialImageCleanup& operator=(const PartialImageCleanup&) = delete;

  void Release() { active_ = false; }

 private:
  const std::string& path_;
  bool active_;
};

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
}

void SetSystemError(std::string* error, std::string_view operation,
                    int error_number) {
  if (error != nullptr) {
    error->assign(operation);
    error->append(": ");
    error->append(std::strerror(error_number));
  }
}

// 循环处理短写和 EINTR，只有完整写入 size 字节才成功。
bool PwriteAll(int fd, const std::uint8_t* data, std::size_t size,
               std::uint64_t offset, std::string* error) {
  std::size_t written = 0;
  while (written < size) {
    const auto result = pwrite(fd, data + written, size - written,
                               static_cast<off_t>(offset + written));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetSystemError(error, "pwrite", errno);
      return false;
    }
    if (result == 0) {
      SetError(error, "pwrite made no progress");
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

// 设置 bitmap 中一个全局位。
void SetBitmapBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit) {
  (*bitmap)[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

// 把 [0, used_bits) 全部标记已占用，用于 block bitmap 保留 metadata 前缀。
void SetBitmapPrefix(std::vector<std::uint8_t>* bitmap,
                     std::uint32_t used_bits) {
  const std::size_t full_bytes = used_bits / 8U;
  std::fill(bitmap->begin(), bitmap->begin() + full_bytes, 0xFFU);
  const std::uint32_t remaining_bits = used_bits % 8U;
  if (remaining_bits != 0) {
    (*bitmap)[full_bytes] =
        static_cast<std::uint8_t>((1U << remaining_bits) - 1U);
  }
}

// request ledger 的物理位置完全由 superblock data 起点和容量推导。
struct RequestLedgerGeometry {
  std::uint32_t entry_count{0};
  std::uint32_t data_block_count{0};
  std::uint32_t root_directory_block{0};
  std::uint32_t first_data_block{0};
  std::uint32_t indirect_block{0};
  std::uint64_t file_size{0};

  bool enabled() const { return entry_count != 0; }
};

bool BuildRequestLedgerGeometry(const ondisk::Superblock& superblock,
                                std::uint32_t entry_count,
                                RequestLedgerGeometry* output,
                                std::string* error) {
  if (output == nullptr) {
    SetError(error, "request ledger geometry output is required");
    return false;
  }
  RequestLedgerGeometry candidate;
  if (entry_count == 0) {
    *output = candidate;
    return true;
  }

  static_assert(ondisk::kBlockSize %
                        object_store::kRequestLedgerRecordSize ==
                    0,
                "request ledger records must divide an EUFS block");
  constexpr std::uint32_t kEntriesPerBlock =
      ondisk::kBlockSize / object_store::kRequestLedgerRecordSize;
  constexpr std::uint32_t kMaximumFileBlocks =
      ondisk::kDirectBlockCount +
      ondisk::kBlockSize / sizeof(std::uint32_t);
  if (entry_count % kEntriesPerBlock != 0) {
    SetError(error, "request ledger entry count must fill complete blocks");
    return false;
  }
  const std::uint32_t data_blocks = entry_count / kEntriesPerBlock;
  if (data_blocks == 0 || data_blocks > kMaximumFileBlocks) {
    SetError(error, "request ledger exceeds the v1 inode mapping limit");
    return false;
  }
  // 除根和 ledger 外至少保留一个 inode，不能格式化出无法创建业务对象的镜像。
  if (superblock.total_inodes <= object_store::kRequestLedgerInodeNumber) {
    SetError(error, "request ledger requires at least three inodes");
    return false;
  }
  const bool needs_indirect = data_blocks > ondisk::kDirectBlockCount;
  const std::uint32_t occupied_data_blocks =
      1U + data_blocks + (needs_indirect ? 1U : 0U);
  // 同样至少保留一个普通 data block 给后续业务对象。
  if (occupied_data_blocks >= superblock.data.block_count) {
    SetError(error, "image data region is too small for request ledger");
    return false;
  }

  candidate.entry_count = entry_count;
  candidate.data_block_count = data_blocks;
  candidate.root_directory_block = superblock.data.start_block;
  candidate.first_data_block = candidate.root_directory_block + 1U;
  candidate.indirect_block =
      needs_indirect ? candidate.first_data_block + data_blocks : 0;
  candidate.file_size =
      static_cast<std::uint64_t>(entry_count) *
      object_store::kRequestLedgerRecordSize;
  *output = candidate;
  return true;
}

// single-indirect 表中的每个块号也按稳定 little-endian ABI 编码。
void PutLe32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

// 把 [valid_bits, bitmap容量) 填充尾部全部置 1，防止分配器越过真实几何。
void ReserveBitmapTail(std::vector<std::uint8_t>* bitmap,
                       std::uint32_t valid_bits) {
  const std::size_t first_tail_byte = valid_bits / 8U;
  const std::uint32_t valid_bits_in_byte = valid_bits % 8U;
  if (valid_bits_in_byte != 0) {
    const std::uint8_t tail_mask =
        static_cast<std::uint8_t>(0xFFU << valid_bits_in_byte);
    (*bitmap)[first_tail_byte] |= tail_mask;
    std::fill(bitmap->begin() + first_tail_byte + 1, bitmap->end(), 0xFFU);
  } else {
    std::fill(bitmap->begin() + first_tail_byte, bitmap->end(), 0xFFU);
  }
}

// 使用 getrandom 完整填充 UUID 原始字节，处理 EINTR/短读。
bool FillRandom(std::uint8_t* output, std::size_t size, std::string* error) {
  std::size_t filled = 0;
  while (filled < size) {
    const auto result = getrandom(output + filled, size - filled, 0);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetSystemError(error, "getrandom", errno);
      return false;
    }
    filled += static_cast<std::size_t>(result);
  }
  return true;
}

// 取得可编码为 uint64 纳秒的 CLOCK_REALTIME。
bool CurrentTimeNs(std::uint64_t* output, std::string* error) {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    SetSystemError(error, "clock_gettime", errno);
    return false;
  }
  if (now.tv_sec < 0 ||
      static_cast<std::uint64_t>(now.tv_sec) >
          (std::numeric_limits<std::uint64_t>::max() -
           static_cast<std::uint64_t>(now.tv_nsec)) /
              1000000000ULL) {
    SetError(error, "current time does not fit the on-disk timestamp");
    return false;
  }
  *output = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(now.tv_nsec);
  return true;
}

}  // 匿名命名空间：格式化辅助资源和函数不对外暴露。

// 按“先完整 body，最后 superblock”顺序创建 EUFS v1 镜像。
bool FormatImage(const MkfsOptions& options,
                 ondisk::Superblock* formatted_superblock,
                 std::string* error) {
  // 镜像容量必须能被固定 4096 字节块整除。
  if (options.image_path.empty() || options.image_size_bytes == 0 ||
      options.image_size_bytes % ondisk::kBlockSize != 0) {
    SetError(error, "image path and block-aligned image size are required");
    return false;
  }
  // v1 物理块号是 uint32_t，同时宿主机 off_t 也必须能表示整个文件。
  const std::uint64_t total_blocks64 =
      options.image_size_bytes / ondisk::kBlockSize;
  if (total_blocks64 > std::numeric_limits<std::uint32_t>::max() ||
      options.image_size_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    SetError(error, "image exceeds the v1 block-number or host offset limit");
    return false;
  }

  // 先纯计算并验证各 metadata/journal/data 区域几何。
  ondisk::Superblock superblock;
  if (!ondisk::BuildSuperblockLayout(
          static_cast<std::uint32_t>(total_blocks64), options.total_inodes,
          options.journal_blocks, &superblock, error)) {
    return false;
  }
  RequestLedgerGeometry ledger_geometry;
  if (!BuildRequestLedgerGeometry(superblock, options.request_ledger_entries,
                                  &ledger_geometry, error)) {
    return false;
  }
  if (ledger_geometry.enabled()) {
    superblock.feature_incompat |= ondisk::kFeatureIncompatRequestLedger;
  }
  // 生成镜像身份和创建时间，再设置 RFC 4122 version/variant 位。
  if (!FillRandom(superblock.filesystem_uuid.data(),
                  superblock.filesystem_uuid.size(), error) ||
      !CurrentTimeNs(&superblock.created_time_ns, error)) {
    return false;
  }
  superblock.filesystem_uuid[6] = static_cast<std::uint8_t>(
      (superblock.filesystem_uuid[6] & 0x0FU) | 0x40U);
  superblock.filesystem_uuid[8] = static_cast<std::uint8_t>(
      (superblock.filesystem_uuid[8] & 0x3FU) | 0x80U);

  // force=false 使用 O_EXCL，避免覆盖现有路径；force=true 允许重建。
  const int flags =
      O_RDWR | O_CREAT | O_CLOEXEC | (options.force ? 0 : O_EXCL);
  const int raw_fd = open(options.image_path.c_str(), flags, 0644);
  if (raw_fd < 0) {
    SetSystemError(error, "open image", errno);
    return false;
  }
  // 新路径创建失败时自动删除部分文件；覆盖既有路径时保留路径本身。
  FileDescriptor fd(raw_fd);
  PartialImageCleanup cleanup(options.image_path, !options.force);
  // 即使是离线格式化，也拒绝与在线挂载/Backend 同时操作同一镜像。
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    SetSystemError(error, "lock image", errno);
    return false;
  }

  // 覆盖旧镜像时先持久化清零 superblock，使后续中途失败镜像不可挂载。
  if (options.force) {
    ondisk::Block invalid_superblock{};
    if (!PwriteAll(fd.get(), invalid_superblock.data(),
                   invalid_superblock.size(), 0, error)) {
      return false;
    }
    if (fdatasync(fd.get()) != 0) {
      SetSystemError(error, "fdatasync invalidated superblock", errno);
      return false;
    }
  }

  // 先设置逻辑文件长度，再要求宿主文件系统真实预分配空间。
  if (ftruncate(fd.get(), static_cast<off_t>(options.image_size_bytes)) != 0) {
    SetSystemError(error, "ftruncate image", errno);
    return false;
  }
  const int allocation_error =
      posix_fallocate(fd.get(), 0, static_cast<off_t>(options.image_size_bytes));
  if (allocation_error != 0) {
    SetSystemError(error, "posix_fallocate image", allocation_error);
    return false;
  }

  // inode bitmap 至少分配根；启用 ledger 时还固定分配 inode 2。
  std::vector<std::uint8_t> inode_bitmap(
      static_cast<std::size_t>(superblock.inode_bitmap.block_count) *
          ondisk::kBlockSize,
      0);
  SetBitmapBit(&inode_bitmap, superblock.root_inode - 1U);
  if (ledger_geometry.enabled()) {
    SetBitmapBit(&inode_bitmap,
                 object_store::kRequestLedgerInodeNumber - 1U);
  }
  ReserveBitmapTail(&inode_bitmap, superblock.total_inodes);

  // block bitmap 把 data.start_block 之前全部 metadata/journal 块标记占用。
  std::vector<std::uint8_t> block_bitmap(
      static_cast<std::size_t>(superblock.block_bitmap.block_count) *
          ondisk::kBlockSize,
      0);
  SetBitmapPrefix(&block_bitmap, superblock.data.start_block);
  if (ledger_geometry.enabled()) {
    SetBitmapBit(&block_bitmap, ledger_geometry.root_directory_block);
    for (std::uint32_t logical = 0;
         logical < ledger_geometry.data_block_count; ++logical) {
      SetBitmapBit(&block_bitmap,
                   ledger_geometry.first_data_block + logical);
    }
    if (ledger_geometry.indirect_block != 0) {
      SetBitmapBit(&block_bitmap, ledger_geometry.indirect_block);
    }
  }
  ReserveBitmapTail(&block_bitmap, superblock.total_blocks);

  // 未启用 ledger 时根目录保持空；启用后根目录只包含受保护的系统文件。
  ondisk::InodeRecord root;
  root.inode_number = superblock.root_inode;
  root.mode = S_IFDIR | 0755;
  root.uid = static_cast<std::uint32_t>(getuid());
  root.gid = static_cast<std::uint32_t>(getgid());
  root.link_count = 2;
  root.atime_ns = superblock.created_time_ns;
  root.mtime_ns = superblock.created_time_ns;
  root.ctime_ns = superblock.created_time_ns;
  root.generation = 1;
  ondisk::Block root_directory_bytes{};
  ondisk::InodeRecord ledger;
  ondisk::InodeBytes ledger_bytes{};
  ondisk::Block ledger_indirect_bytes{};
  if (ledger_geometry.enabled()) {
    root.size = ondisk::kBlockSize;
    root.direct_blocks[0] = ledger_geometry.root_directory_block;

    ledger.inode_number = object_store::kRequestLedgerInodeNumber;
    ledger.mode = S_IFREG | 0600;
    ledger.uid = root.uid;
    ledger.gid = root.gid;
    ledger.link_count = 1;
    ledger.size = ledger_geometry.file_size;
    ledger.atime_ns = superblock.created_time_ns;
    ledger.mtime_ns = superblock.created_time_ns;
    ledger.ctime_ns = superblock.created_time_ns;
    ledger.generation = 1;
    const std::uint32_t direct_count = std::min<std::uint32_t>(
        ledger_geometry.data_block_count, ondisk::kDirectBlockCount);
    for (std::uint32_t logical = 0; logical < direct_count; ++logical) {
      ledger.direct_blocks[logical] =
          ledger_geometry.first_data_block + logical;
    }
    ledger.indirect_block = ledger_geometry.indirect_block;
    for (std::uint32_t logical = ondisk::kDirectBlockCount;
         logical < ledger_geometry.data_block_count; ++logical) {
      const std::uint32_t indirect_index =
          logical - static_cast<std::uint32_t>(ondisk::kDirectBlockCount);
      PutLe32(ledger_indirect_bytes.data() +
                  static_cast<std::size_t>(indirect_index) *
                      sizeof(std::uint32_t),
              ledger_geometry.first_data_block + logical);
    }

    ondisk::DirectoryEntry ledger_entry;
    ledger_entry.inode = object_store::kRequestLedgerInodeNumber;
    ledger_entry.file_type = ondisk::DirectoryFileType::kRegular;
    ledger_entry.name.assign(object_store::kRequestLedgerName);
    if (!ondisk::EncodeDirectoryEntry(
            ledger_entry, static_cast<std::uint16_t>(ondisk::kBlockSize),
            root_directory_bytes.data(), root_directory_bytes.size(), error) ||
        !ondisk::EncodeInode(ledger, &ledger_bytes, error)) {
      return false;
    }
  }
  ondisk::InodeBytes root_bytes{};
  if (!ondisk::EncodeInode(root, &root_bytes, error)) {
    return false;
  }

  // A/B 两份 control 初始完全相同，表示空 ring 和事务号 1。
  journal::JournalControl clean_control;
  clean_control.ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  clean_control.filesystem_uuid = superblock.filesystem_uuid;
  clean_control.generation = 0;
  clean_control.head = 0;
  clean_control.tail = 0;
  clean_control.used_blocks = 0;
  clean_control.next_transaction_id = 1;
  ondisk::Block control_bytes{};
  if (!journal::EncodeControl(clean_control, &control_bytes, nullptr, error)) {
    return false;
  }

  // 先写两个 bitmap、inode 和两份 control，此时块 0 superblock 仍无效。
  if (!PwriteAll(fd.get(), inode_bitmap.data(), inode_bitmap.size(),
                 static_cast<std::uint64_t>(
                     superblock.inode_bitmap.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), block_bitmap.data(), block_bitmap.size(),
                 static_cast<std::uint64_t>(
                     superblock.block_bitmap.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), root_bytes.data(), root_bytes.size(),
                 static_cast<std::uint64_t>(
                     superblock.inode_table.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), control_bytes.data(), control_bytes.size(),
                 static_cast<std::uint64_t>(superblock.journal.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), control_bytes.data(), control_bytes.size(),
                 static_cast<std::uint64_t>(
                     superblock.journal.start_block + 1U) *
                     ondisk::kBlockSize,
                 error)) {
    return false;
  }
  if (ledger_geometry.enabled()) {
    const std::uint64_t inode_table_start =
        static_cast<std::uint64_t>(superblock.inode_table.start_block) *
        ondisk::kBlockSize;
    if (!PwriteAll(
            fd.get(), ledger_bytes.data(), ledger_bytes.size(),
            inode_table_start +
                (object_store::kRequestLedgerInodeNumber - 1U) *
                    ondisk::kInodeRecordSize,
            error) ||
        !PwriteAll(fd.get(), root_directory_bytes.data(),
                   root_directory_bytes.size(),
                   static_cast<std::uint64_t>(
                       ledger_geometry.root_directory_block) *
                       ondisk::kBlockSize,
                   error)) {
      return false;
    }

    // force 重建同尺寸旧镜像时不能依赖 ftruncate 清零，所有 ledger 槽位必须显式清零。
    const ondisk::Block empty_ledger_block{};
    for (std::uint32_t logical = 0;
         logical < ledger_geometry.data_block_count; ++logical) {
      if (!PwriteAll(fd.get(), empty_ledger_block.data(),
                     empty_ledger_block.size(),
                     static_cast<std::uint64_t>(
                         ledger_geometry.first_data_block + logical) *
                         ondisk::kBlockSize,
                     error)) {
        return false;
      }
    }
    if (ledger_geometry.indirect_block != 0 &&
        !PwriteAll(fd.get(), ledger_indirect_bytes.data(),
                   ledger_indirect_bytes.size(),
                   static_cast<std::uint64_t>(ledger_geometry.indirect_block) *
                       ondisk::kBlockSize,
                   error)) {
      return false;
    }
  }
  // 先确认整个文件系统 body 持久化，再允许发布 superblock。
  if (fdatasync(fd.get()) != 0) {
    SetSystemError(error, "fdatasync filesystem body", errno);
    return false;
  }

  // superblock 是最后的发布点：只有它完整编码、写入并 fdatasync 后镜像才有效。
  ondisk::Block superblock_bytes{};
  if (!ondisk::EncodeSuperblock(superblock, &superblock_bytes, error) ||
      !PwriteAll(fd.get(), superblock_bytes.data(), superblock_bytes.size(), 0,
                 error)) {
    return false;
  }
  if (fdatasync(fd.get()) != 0) {
    SetSystemError(error, "fdatasync superblock", errno);
    return false;
  }

  // 可选返回实际格式化几何；最后解除新文件失败清理责任。
  if (formatted_superblock != nullptr) {
    *formatted_superblock = superblock;
  }
  cleanup.Release();
  return true;
}

}  // namespace eufs::storage
