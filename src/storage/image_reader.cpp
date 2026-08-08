#include "storage/image_reader.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unordered_set>
#include <unistd.h>

namespace eufs::storage {
namespace {

// 本文件是磁盘读取唯一高层入口：先验证镜像全局几何，再按需解析 inode/目录/文件。
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
  int Release() {
    const int value = value_;
    value_ = -1;
    return value;
  }

 private:
  int value_;
};

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

// 从 little-endian single-indirect 块读取一个物理块号。
std::uint32_t GetLe32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

// 查询缓存 bitmap 的一个位。
bool BitmapBit(const std::vector<std::uint8_t>& bitmap, std::uint32_t bit) {
  return (bitmap[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) !=
         0;
}

// 交叉验证目录项冗余 file_type 与目标 inode.mode。
bool IsDirectoryType(ondisk::DirectoryFileType type, std::uint32_t mode) {
  if (type == ondisk::DirectoryFileType::kRegular) {
    return S_ISREG(mode);
  }
  if (type == ondisk::DirectoryFileType::kDirectory) {
    return S_ISDIR(mode);
  }
  return false;
}

// 循环 pread 处理短读/EINTR，完整读取指定字节区间。
int ReadExactAt(int fd, std::uint64_t offset, std::uint8_t* output,
                std::size_t size, std::string_view operation,
                std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result =
        pread(fd, output + completed, size - completed,
              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, operation, error_number);
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

}  // 匿名命名空间。

// 私有构造函数接管已经完成全局校验的 fd 和缓存结构。
ImageReader::ImageReader(int fd, std::uint64_t image_size,
                         const ondisk::Superblock& superblock,
                         const journal::JournalControl& journal_control,
                         journal::ControlCopy journal_control_copy,
                         std::vector<std::uint8_t> inode_bitmap,
                         std::vector<std::uint8_t> block_bitmap)
    : fd_(fd),
      image_size_(image_size),
      superblock_(superblock),
      journal_control_(journal_control),
      journal_control_copy_(journal_control_copy),
      inode_bitmap_(std::move(inode_bitmap)),
      block_bitmap_(std::move(block_bitmap)) {}

// 析构关闭 reader 自己接管的复制 fd，不影响 session 主 fd。
ImageReader::~ImageReader() {
  if (fd_ >= 0) {
    close(fd_);
  }
}

// 独立只读打开路径，然后复用 AdoptLockedFd 的全部校验逻辑。
int ImageReader::Open(const std::string& path,
                      std::unique_ptr<ImageReader>* output,
                      std::string* detail) {
  if (path.empty() || output == nullptr) {
    SetDetail(detail, "image path and output are required");
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    SetSystemDetail(detail, "open image", errno);
    return -errno;
  }
  if (flock(fd, LOCK_SH | LOCK_NB) != 0) {
    const int error_number = errno;
    close(fd);
    SetSystemDetail(detail, "lock image", error_number);
    return -error_number;
  }

  return AdoptLockedFd(fd, output, detail);
}

// 接管 fd，读取并验证 superblock、A/B control、bitmap 和 metadata 分配前缀。
int ImageReader::AdoptLockedFd(int locked_fd,
                               std::unique_ptr<ImageReader>* output,
                               std::string* detail) {
  FileDescriptor owned_fd(locked_fd);
  if (output == nullptr || locked_fd < 0) {
    SetDetail(detail, "valid locked fd and reader output are required");
    return -EINVAL;
  }
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  const int status_flags = fcntl(owned_fd.get(), F_GETFL);
  if (status_flags < 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "inspect adopted reader fd", error_number);
    return -error_number;
  }
  if ((status_flags & O_ACCMODE) == O_WRONLY) {
    SetDetail(detail, "adopted reader fd is not open for reading");
    return -EACCES;
  }
  const int fd = owned_fd.get();

  struct stat image_stat {};
  if (fstat(fd, &image_stat) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "fstat image", error_number);
    return -error_number;
  }
  if (image_stat.st_size < static_cast<off_t>(ondisk::kBlockSize) ||
      image_stat.st_size % ondisk::kBlockSize != 0) {
    SetDetail(detail, "image size is not a positive 4 KiB multiple");
    return -EUCLEAN;
  }

  ondisk::Block superblock_bytes{};
  std::size_t read_bytes = 0;
  while (read_bytes < superblock_bytes.size()) {
    const auto result = pread(fd, superblock_bytes.data() + read_bytes,
                              superblock_bytes.size() - read_bytes,
                              static_cast<off_t>(read_bytes));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, "read superblock", error_number);
      return -error_number;
    }
    read_bytes += static_cast<std::size_t>(result);
  }

  ondisk::Superblock superblock;
  if (!ondisk::DecodeSuperblock(superblock_bytes, &superblock, detail)) {
    return -EUCLEAN;
  }
  if ((superblock.feature_incompat &
       ~ondisk::kSupportedFeatureIncompat) != 0) {
    SetDetail(detail, "image requires unsupported incompatible features");
    return -EOPNOTSUPP;
  }
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(superblock.total_blocks) *
      ondisk::kBlockSize;
  if (expected_size != static_cast<std::uint64_t>(image_stat.st_size)) {
    SetDetail(detail, "image size does not match the superblock");
    return -EUCLEAN;
  }

  ondisk::Block control_a{};
  ondisk::Block control_b{};
  const std::uint64_t control_a_offset =
      static_cast<std::uint64_t>(superblock.journal.start_block) *
      ondisk::kBlockSize;
  int result = ReadExactAt(fd, control_a_offset, control_a.data(),
                           control_a.size(), "read journal control A", detail);
  if (result == 0) {
    result = ReadExactAt(fd, control_a_offset + ondisk::kBlockSize,
                         control_b.data(), control_b.size(),
                         "read journal control B", detail);
  }
  if (result != 0) {
    return result;
  }

  journal::JournalControl journal_control;
  journal::ControlCopy journal_control_copy{};
  const std::uint32_t expected_ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  if (!journal::SelectControl(
          control_a, control_b, superblock.filesystem_uuid,
          expected_ring_blocks, &journal_control, &journal_control_copy,
          detail)) {
    return -EUCLEAN;
  }

  auto read_region = [fd, detail](const ondisk::Region& region,
                                  std::vector<std::uint8_t>* bytes) -> int {
    bytes->assign(static_cast<std::size_t>(region.block_count) *
                      ondisk::kBlockSize,
                  0);
    std::size_t completed = 0;
    const std::uint64_t base =
        static_cast<std::uint64_t>(region.start_block) * ondisk::kBlockSize;
    while (completed < bytes->size()) {
      const auto result =
          pread(fd, bytes->data() + completed, bytes->size() - completed,
                static_cast<off_t>(base + completed));
      if (result < 0 && errno == EINTR) {
        continue;
      }
      if (result <= 0) {
        const int error_number = result < 0 ? errno : EIO;
        SetSystemDetail(detail, "read bitmap", error_number);
        return -error_number;
      }
      completed += static_cast<std::size_t>(result);
    }
    return 0;
  };

  std::vector<std::uint8_t> inode_bitmap;
  std::vector<std::uint8_t> block_bitmap;
  result = read_region(superblock.inode_bitmap, &inode_bitmap);
  if (result == 0) {
    result = read_region(superblock.block_bitmap, &block_bitmap);
  }
  if (result != 0) {
    return result;
  }

  if (!BitmapBit(inode_bitmap, superblock.root_inode - 1U)) {
    SetDetail(detail, "root inode is not allocated in the inode bitmap");
    return -EUCLEAN;
  }
  for (std::uint32_t block = 0; block < superblock.data.start_block; ++block) {
    if (!BitmapBit(block_bitmap, block)) {
      SetDetail(detail, "metadata block is free in the block bitmap");
      return -EUCLEAN;
    }
  }
  const std::uint64_t inode_bitmap_bits = inode_bitmap.size() * 8ULL;
  for (std::uint64_t inode = superblock.total_inodes;
       inode < inode_bitmap_bits; ++inode) {
    if (!BitmapBit(inode_bitmap, static_cast<std::uint32_t>(inode))) {
      SetDetail(detail, "inode bitmap tail contains allocatable bits");
      return -EUCLEAN;
    }
  }
  const std::uint64_t block_bitmap_bits = block_bitmap.size() * 8ULL;
  for (std::uint64_t block = superblock.total_blocks;
       block < block_bitmap_bits; ++block) {
    if (!BitmapBit(block_bitmap, static_cast<std::uint32_t>(block))) {
      SetDetail(detail, "block bitmap tail contains allocatable bits");
      return -EUCLEAN;
    }
  }

  auto reader = std::unique_ptr<ImageReader>(new ImageReader(
      owned_fd.Release(), expected_size, superblock, journal_control,
      journal_control_copy, std::move(inode_bitmap), std::move(block_bitmap)));
  ondisk::InodeRecord root;
  result = reader->ReadInode(superblock.root_inode, &root, detail);
  if (result != 0) {
    return result == -ENOENT ? -EUCLEAN : result;
  }
  if (!S_ISDIR(root.mode)) {
    SetDetail(detail, "root inode is not a directory");
    return -EUCLEAN;
  }

  *output = std::move(reader);
  return 0;
}

// 以下两个函数只查询打开时缓存的 bitmap 快照。
bool ImageReader::IsInodeAllocated(std::uint32_t inode_number) const {
  return inode_number >= 1 && inode_number <= superblock_.total_inodes &&
         BitmapBit(inode_bitmap_, inode_number - 1U);
}

bool ImageReader::IsBlockAllocated(std::uint32_t block_number) const {
  return block_number < superblock_.total_blocks &&
         BitmapBit(block_bitmap_, block_number);
}

// 读取任意镜像物理块，先做范围和输出地址检查。
int ImageReader::ReadBlock(std::uint32_t block_number, ondisk::Block* output,
                           std::string* detail) const {
  if (output == nullptr || block_number >= superblock_.total_blocks) {
    return -EINVAL;
  }
  return ReadExact(static_cast<std::uint64_t>(block_number) *
                       ondisk::kBlockSize,
                   output->data(), output->size(), detail);
}

int ImageReader::ReadExact(std::uint64_t offset, std::uint8_t* output,
                           std::size_t size, std::string* detail) const {
  if (output == nullptr || offset > image_size_ ||
      size > image_size_ - offset) {
    SetDetail(detail, "requested image range is outside the image");
    return -EUCLEAN;
  }
  std::size_t completed = 0;
  while (completed < size) {
    const auto result =
        pread(fd_, output + completed, size - completed,
              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, "pread image", error_number);
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

// 验证数据块位于 data 区并在 bitmap 中标记占用。
int ImageReader::ValidateAllocatedDataBlock(std::uint32_t block_number,
                                            std::string* detail) const {
  if (block_number < superblock_.data.start_block ||
      block_number >= superblock_.total_blocks ||
      !IsBlockAllocated(block_number)) {
    SetDetail(detail, "inode references an invalid or unallocated data block");
    return -EUCLEAN;
  }
  return 0;
}

// logical < 12 使用 inode.direct；其余从 inode.indirect_block 读取 uint32 指针。
int ImageReader::MapLogicalBlock(const ondisk::InodeRecord& inode,
                                 std::uint32_t logical_block,
                                 std::uint32_t* physical_block,
                                 std::string* detail) const {
  if (physical_block == nullptr) {
    return -EINVAL;
  }
  if (logical_block < ondisk::kDirectBlockCount) {
    *physical_block = inode.direct_blocks[logical_block];
  } else {
    const std::uint32_t indirect_index =
        logical_block - ondisk::kDirectBlockCount;
    if (indirect_index >= ondisk::kBlockSize / sizeof(std::uint32_t) ||
        inode.indirect_block == 0) {
      SetDetail(detail, "logical block exceeds the v1 inode mapping");
      return -EUCLEAN;
    }
    int result = ValidateAllocatedDataBlock(inode.indirect_block, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block indirect{};
    result = ReadExact(
        static_cast<std::uint64_t>(inode.indirect_block) * ondisk::kBlockSize,
        indirect.data(), indirect.size(), detail);
    if (result != 0) {
      return result;
    }
    *physical_block =
        GetLe32(indirect.data() + indirect_index * sizeof(std::uint32_t));
  }
  if (*physical_block == 0) {
    SetDetail(detail, "allocated file range contains a sparse block");
    return -EUCLEAN;
  }
  return ValidateAllocatedDataBlock(*physical_block, detail);
}

// 定位 inode table 槽位，读取 128 字节并调用统一 DecodeInode。
int ImageReader::ReadInode(std::uint32_t inode_number,
                           ondisk::InodeRecord* output,
                           std::string* detail) const {
  if (output == nullptr || inode_number == 0 ||
      inode_number > superblock_.total_inodes) {
    return -EINVAL;
  }
  if (!IsInodeAllocated(inode_number)) {
    return -ENOENT;
  }
  const std::uint64_t index = inode_number - 1U;
  const std::uint64_t offset =
      static_cast<std::uint64_t>(superblock_.inode_table.start_block) *
          ondisk::kBlockSize +
      index * ondisk::kInodeRecordSize;
  ondisk::InodeBytes bytes{};
  int result = ReadExact(offset, bytes.data(), bytes.size(), detail);
  if (result != 0) {
    return result;
  }
  if (!ondisk::DecodeInode(bytes, inode_number, output, detail)) {
    return -EUCLEAN;
  }

  const std::uint32_t required_blocks = static_cast<std::uint32_t>(
      (output->size + ondisk::kBlockSize - 1U) / ondisk::kBlockSize);
  if (S_ISDIR(output->mode) && output->size % ondisk::kBlockSize != 0) {
    SetDetail(detail, "directory size is not a whole number of blocks");
    return -EUCLEAN;
  }
  const std::uint32_t required_direct = std::min<std::uint32_t>(
      required_blocks, ondisk::kDirectBlockCount);
  for (std::size_t index_direct = 0;
       index_direct < output->direct_blocks.size(); ++index_direct) {
    if (index_direct < required_direct) {
      if (output->direct_blocks[index_direct] == 0) {
        SetDetail(detail, "allocated file range contains a sparse direct block");
        return -EUCLEAN;
      }
      result =
          ValidateAllocatedDataBlock(output->direct_blocks[index_direct], detail);
      if (result != 0) {
        return result;
      }
    } else if (output->direct_blocks[index_direct] != 0) {
      SetDetail(detail, "inode contains a direct block beyond end of file");
      return -EUCLEAN;
    }
  }
  if (required_blocks <= ondisk::kDirectBlockCount) {
    if (output->indirect_block != 0) {
      SetDetail(detail, "inode contains an unnecessary indirect block");
      return -EUCLEAN;
    }
    return 0;
  }

  if (output->indirect_block == 0) {
    SetDetail(detail, "inode is missing its required indirect block");
    return -EUCLEAN;
  }
  result = ValidateAllocatedDataBlock(output->indirect_block, detail);
  if (result != 0) {
    return result;
  }
  ondisk::Block indirect{};
  result = ReadExact(
      static_cast<std::uint64_t>(output->indirect_block) *
          ondisk::kBlockSize,
      indirect.data(), indirect.size(), detail);
  if (result != 0) {
    return result;
  }
  const std::uint32_t required_indirect =
      required_blocks - ondisk::kDirectBlockCount;
  const std::uint32_t indirect_entries =
      ondisk::kBlockSize / sizeof(std::uint32_t);
  for (std::uint32_t index_indirect = 0; index_indirect < indirect_entries;
       ++index_indirect) {
    const std::uint32_t block =
        GetLe32(indirect.data() + index_indirect * sizeof(std::uint32_t));
    if (index_indirect < required_indirect) {
      if (block == 0) {
        SetDetail(detail,
                  "allocated file range contains a sparse indirect block");
        return -EUCLEAN;
      }
      result = ValidateAllocatedDataBlock(block, detail);
      if (result != 0) {
        return result;
      }
    } else if (block != 0) {
      SetDetail(detail, "indirect block contains a pointer beyond end of file");
      return -EUCLEAN;
    }
  }
  return 0;
}

// 按目录 inode 的逻辑块顺序解码可变长记录；一个坏 record 会使该目录读取失败。
int ImageReader::ListDirectory(
    std::uint32_t inode_number,
    std::vector<ondisk::DirectoryEntry>* output,
    std::string* detail) const {
  if (output == nullptr) {
    return -EINVAL;
  }
  output->clear();
  ondisk::InodeRecord directory;
  int result = ReadInode(inode_number, &directory, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISDIR(directory.mode)) {
    return -ENOTDIR;
  }

  std::unordered_set<std::string> names;
  const std::uint32_t block_count =
      static_cast<std::uint32_t>(directory.size / ondisk::kBlockSize);
  for (std::uint32_t logical = 0; logical < block_count; ++logical) {
    std::uint32_t physical = 0;
    result = MapLogicalBlock(directory, logical, &physical, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block block{};
    result = ReadExact(static_cast<std::uint64_t>(physical) *
                           ondisk::kBlockSize,
                       block.data(), block.size(), detail);
    if (result != 0) {
      return result;
    }

    std::size_t offset = 0;
    while (offset < block.size()) {
      ondisk::DirectoryEntry entry;
      if (!ondisk::DecodeDirectoryEntry(block.data() + offset,
                                        block.size() - offset, &entry,
                                        detail)) {
        return -EUCLEAN;
      }
      if (entry.record_length == 0) {
        SetDetail(detail, "directory record made no progress");
        return -EUCLEAN;
      }
      offset += entry.record_length;
      if (entry.inode == 0) {
        continue;
      }
      if (!names.insert(entry.name).second) {
        SetDetail(detail, "directory contains a duplicate active name");
        return -EUCLEAN;
      }
      ondisk::InodeRecord child;
      result = ReadInode(entry.inode, &child, detail);
      if (result == -ENOENT) {
        SetDetail(detail, "directory entry points to an unallocated inode");
        return -EUCLEAN;
      }
      if (result != 0) {
        return result;
      }
      if (!IsDirectoryType(entry.file_type, child.mode)) {
        SetDetail(detail, "directory entry type disagrees with the inode mode");
        return -EUCLEAN;
      }
      output->push_back(std::move(entry));
    }
  }
  return 0;
}

// 从根 inode 开始逐分量解析绝对路径，不接受空分量、`.` 或 `..`。
int ImageReader::ResolvePath(std::string_view path,
                             std::uint32_t* inode_number,
                             ondisk::InodeRecord* inode,
                             std::string* detail) const {
  if (path.empty() || path.front() != '/' || inode_number == nullptr ||
      inode == nullptr) {
    return -EINVAL;
  }
  std::uint32_t current = superblock_.root_inode;
  if (path == "/") {
    const int result = ReadInode(current, inode, detail);
    if (result == 0) {
      *inode_number = current;
    }
    return result;
  }
  if (path.back() == '/') {
    return -EINVAL;
  }

  std::size_t component_start = 1;
  while (component_start < path.size()) {
    const std::size_t slash = path.find('/', component_start);
    const std::size_t component_end =
        slash == std::string_view::npos ? path.size() : slash;
    const std::string_view component =
        path.substr(component_start, component_end - component_start);
    if (component.empty() || component == "." || component == "..") {
      return -EINVAL;
    }

    std::vector<ondisk::DirectoryEntry> entries;
    int result = ListDirectory(current, &entries, detail);
    if (result != 0) {
      return result;
    }
    const auto found = std::find_if(
        entries.begin(), entries.end(), [component](const auto& entry) {
          return entry.name == component;
        });
    if (found == entries.end()) {
      return -ENOENT;
    }
    current = found->inode;
    if (slash == std::string_view::npos) {
      break;
    }
    component_start = slash + 1;
  }

  const int result = ReadInode(current, inode, detail);
  if (result == 0) {
    *inode_number = current;
  }
  return result;
}

// 按 offset 和 requested 跨逻辑块读取，EOF 外请求返回 0 字节。
int ImageReader::ReadFile(std::uint32_t inode_number, std::uint64_t offset,
                          std::uint8_t* output, std::size_t requested,
                          std::size_t* bytes_read,
                          std::string* detail) const {
  if ((output == nullptr && requested != 0) || bytes_read == nullptr) {
    return -EINVAL;
  }
  *bytes_read = 0;
  ondisk::InodeRecord inode;
  int result = ReadInode(inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISREG(inode.mode)) {
    return -EISDIR;
  }
  if (offset >= inode.size || requested == 0) {
    return 0;
  }
  const std::size_t total = static_cast<std::size_t>(std::min<std::uint64_t>(
      requested, inode.size - offset));
  while (*bytes_read < total) {
    const std::uint64_t current_offset = offset + *bytes_read;
    const std::uint32_t logical =
        static_cast<std::uint32_t>(current_offset / ondisk::kBlockSize);
    const std::size_t inside =
        static_cast<std::size_t>(current_offset % ondisk::kBlockSize);
    std::uint32_t physical = 0;
    result = MapLogicalBlock(inode, logical, &physical, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block block{};
    result = ReadExact(static_cast<std::uint64_t>(physical) *
                           ondisk::kBlockSize,
                       block.data(), block.size(), detail);
    if (result != 0) {
      return result;
    }
    const std::size_t chunk =
        std::min(total - *bytes_read, block.size() - inside);
    std::copy_n(block.data() + inside, chunk, output + *bytes_read);
    *bytes_read += chunk;
  }
  return 0;
}

}  // namespace eufs::storage
  // candidate 局部收集全部状态，任一步失败都不会发布半初始化 reader。
  // superblock 决定后续所有区域偏移，必须最先读取并验证。
  // A/B control 必须与 superblock UUID/ring 几何匹配。
  // inode/block bitmap 完整载入内存，并验证 metadata 前缀与容量尾部均保留。
  // 根据 inode.size 验证必需指针存在、末尾多余指针为 0、所有块已分配且不重复。
