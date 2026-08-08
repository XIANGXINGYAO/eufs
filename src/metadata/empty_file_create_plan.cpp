#include "metadata/empty_file_create_plan.h"

#include "storage/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace eufs::metadata {
namespace {

// 写入可选计划错误详情。
void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

// 把一个连续 metadata 区域逐块加载为字节数组，供 bitmap allocator 使用。
int LoadRegion(const storage::ImageReader& image, const ondisk::Region& region,
               std::vector<std::uint8_t>* output, std::string* detail) {
  output->assign(static_cast<std::size_t>(region.block_count) *
                     ondisk::kBlockSize,
                 0);
  for (std::uint32_t index = 0; index < region.block_count; ++index) {
    ondisk::Block block{};
    const int result = image.ReadBlock(region.start_block + index, &block,
                                       detail);
    if (result != 0) {
      return result;
    }
    std::copy(block.begin(), block.end(),
              output->begin() +
                  static_cast<std::size_t>(index) * ondisk::kBlockSize);
  }
  return 0;
}

// 从完整 bitmap 字节数组取出一个 4096 字节 home block。
void CopyBitmapBlock(const std::vector<std::uint8_t>& bitmap,
                     std::uint32_t local_block, ondisk::Block* output) {
  const auto begin = bitmap.begin() +
                     static_cast<std::size_t>(local_block) *
                         ondisk::kBlockSize;
  std::copy_n(begin, ondisk::kBlockSize, output->begin());
}

// 首次修改某 metadata 块时同时保存 before-image，并返回可继续编辑的 after-image。
int MutableExistingBlock(const storage::ImageReader& image,
                         std::uint32_t block_number,
                         std::map<std::uint32_t, ondisk::Block>* blocks,
                         ondisk::Block** output, std::string* detail) {
  auto [entry, inserted] = blocks->try_emplace(block_number);
  if (inserted) {
    const int result = image.ReadBlock(block_number, &entry->second, detail);
    if (result != 0) {
      blocks->erase(entry);
      return result;
    }
  }
  *output = &entry->second;
  return 0;
}

// 根据固定 128 字节 inode 记录计算其所在 inode table 物理块及块内偏移。
std::uint32_t InodeTableBlock(const ondisk::Superblock& superblock,
                              std::uint32_t inode_number) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(inode_number - 1U) *
      ondisk::kInodeRecordSize;
  return superblock.inode_table.start_block +
         static_cast<std::uint32_t>(byte_index / ondisk::kBlockSize);
}

std::size_t InodeOffsetInBlock(std::uint32_t inode_number) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(inode_number - 1U) *
      ondisk::kInodeRecordSize;
  return static_cast<std::size_t>(byte_index % ondisk::kBlockSize);
}

}  // 匿名命名空间：计划构造辅助函数不导出。

// 在根目录已有数据块中插入新目录项，并联合规划 inode/目录/bitmap after-image。
int PrepareRootEmptyFileCreate(const storage::ImageReader& image,
                               std::string_view name,
                               std::uint32_t permissions, std::uint32_t uid,
                               std::uint32_t gid, std::uint64_t timestamp_ns,
                               EmptyFileCreatePlan* output,
                               std::string* detail) {
  if (output == nullptr) {
    return -EINVAL;
  }
  if (detail != nullptr) {
    detail->clear();
  }

  const auto& superblock = image.superblock();
  ondisk::InodeRecord root;
  int result = image.ReadInode(superblock.root_inode, &root, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISDIR(root.mode)) {
    SetDetail(detail, "root inode is not a directory");
    return -EUCLEAN;
  }
  if (root.size != 0 || root.indirect_block != 0 ||
      std::any_of(root.direct_blocks.begin(), root.direct_blocks.end(),
                  [](std::uint32_t block) { return block != 0; })) {
    SetDetail(detail, "first create planner currently requires an empty root");
    return -EOPNOTSUPP;
  }

  std::vector<std::uint8_t> inode_bitmap;
  std::vector<std::uint8_t> block_bitmap;
  result = LoadRegion(image, superblock.inode_bitmap, &inode_bitmap, detail);
  if (result == 0) {
    result = LoadRegion(image, superblock.block_bitmap, &block_bitmap, detail);
  }
  if (result != 0) {
    return result;
  }

  storage::BitmapAllocator inode_allocator(&inode_bitmap,
                                            superblock.total_inodes, 0);
  storage::BitmapAllocator block_allocator(
      &block_bitmap, superblock.total_blocks, superblock.data.start_block);
  std::string allocator_error;
  if (!inode_allocator.Validate(&allocator_error) ||
      !block_allocator.Validate(&allocator_error)) {
    SetDetail(detail, allocator_error);
    return -EUCLEAN;
  }

  storage::BitmapReservation inode_reservation;
  storage::BitmapReservation block_reservation;
  result = inode_allocator.Reserve(&inode_reservation, detail);
  if (result != 0) {
    return result;
  }
  result = block_allocator.Reserve(&block_reservation, detail);
  if (result != 0) {
    return result;
  }

  EmptyFileCreatePlan candidate;
  candidate.inode_number = inode_reservation.bit() + 1U;
  candidate.directory_block = block_reservation.bit();
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;

  const std::uint32_t inode_bitmap_local =
      inode_reservation.bit() / (ondisk::kBlockSize * 8U);
  const std::uint32_t inode_bitmap_home =
      superblock.inode_bitmap.start_block + inode_bitmap_local;
  result = image.ReadBlock(inode_bitmap_home,
                           &candidate.before_images[inode_bitmap_home], detail);
  if (result != 0) {
    return result;
  }
  CopyBitmapBlock(inode_bitmap, inode_bitmap_local,
                  &candidate.after_images[inode_bitmap_home]);

  const std::uint32_t block_bitmap_local =
      block_reservation.bit() / (ondisk::kBlockSize * 8U);
  const std::uint32_t block_bitmap_home =
      superblock.block_bitmap.start_block + block_bitmap_local;
  result = image.ReadBlock(block_bitmap_home,
                           &candidate.before_images[block_bitmap_home], detail);
  if (result != 0) {
    return result;
  }
  CopyBitmapBlock(block_bitmap, block_bitmap_local,
                  &candidate.after_images[block_bitmap_home]);

  const std::uint32_t root_table_block =
      InodeTableBlock(superblock, superblock.root_inode);
  ondisk::Block* root_table_after = nullptr;
  result = MutableExistingBlock(image, root_table_block,
                                &candidate.after_images, &root_table_after,
                                detail);
  if (result != 0) {
    return result;
  }
  candidate.before_images[root_table_block] = *root_table_after;

  root.size = ondisk::kBlockSize;
  root.direct_blocks[0] = candidate.directory_block;
  root.mtime_ns = timestamp_ns;
  root.ctime_ns = timestamp_ns;
  ondisk::InodeBytes root_bytes{};
  if (!ondisk::EncodeInode(root, &root_bytes, detail)) {
    return -EUCLEAN;
  }
  std::copy(root_bytes.begin(), root_bytes.end(),
            root_table_after->begin() +
                InodeOffsetInBlock(superblock.root_inode));

  const std::uint32_t file_table_block =
      InodeTableBlock(superblock, candidate.inode_number);
  ondisk::Block* file_table_after = nullptr;
  result = MutableExistingBlock(image, file_table_block,
                                &candidate.after_images, &file_table_after,
                                detail);
  if (result != 0) {
    return result;
  }
  ondisk::InodeRecord file;
  file.inode_number = candidate.inode_number;
  file.mode = S_IFREG | (permissions & 0777U);
  file.uid = uid;
  file.gid = gid;
  file.link_count = 1;
  file.atime_ns = timestamp_ns;
  file.mtime_ns = timestamp_ns;
  file.ctime_ns = timestamp_ns;
  file.generation = 1;
  ondisk::InodeBytes file_bytes{};
  if (!ondisk::EncodeInode(file, &file_bytes, detail)) {
    return -EINVAL;
  }
  std::copy(file_bytes.begin(), file_bytes.end(),
            file_table_after->begin() +
                InodeOffsetInBlock(candidate.inode_number));

  ondisk::DirectoryEntry entry;
  entry.inode = candidate.inode_number;
  entry.file_type = ondisk::DirectoryFileType::kRegular;
  entry.name.assign(name);
  result = image.ReadBlock(candidate.directory_block,
                           &candidate.before_images[candidate.directory_block],
                           detail);
  if (result != 0) {
    return result;
  }
  ondisk::Block directory_after{};
  if (!ondisk::EncodeDirectoryEntry(
          entry, static_cast<std::uint16_t>(ondisk::kBlockSize),
          directory_after.data(), directory_after.size(), detail)) {
    return -EINVAL;
  }
  candidate.after_images[candidate.directory_block] = directory_after;
  candidate.write_order = {candidate.directory_block, inode_bitmap_home,
                           block_bitmap_home, root_table_block};

  inode_reservation.KeepReserved();
  block_reservation.KeepReserved();
  *output = std::move(candidate);
  return 0;
}

}  // namespace eufs::metadata
  // planner 只接受单层合法名称和 POSIX 权限低 9 位。
  // 先确认同名路径确实不存在；其他解析错误不能被当作可创建。
  // inode bitmap reservation 使用 RAII，后续任一步失败都会自动回滚内存位。
  // before/after 最终完整后才 KeepReserved 并发布 candidate。
