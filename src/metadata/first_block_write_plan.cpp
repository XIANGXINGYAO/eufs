#include "metadata/first_block_write_plan.h"

#include "storage/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace eufs::metadata {
namespace {

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

int LoadRegion(const storage::ImageReader& image, const ondisk::Region& region,
               std::vector<std::uint8_t>* output, std::string* detail) {
  output->assign(static_cast<std::size_t>(region.block_count) *
                     ondisk::kBlockSize,
                 0);
  for (std::uint32_t index = 0; index < region.block_count; ++index) {
    ondisk::Block block{};
    const int result =
        image.ReadBlock(region.start_block + index, &block, detail);
    if (result != 0) {
      return result;
    }
    std::copy(block.begin(), block.end(),
              output->begin() +
                  static_cast<std::size_t>(index) * ondisk::kBlockSize);
  }
  return 0;
}

void CopyBitmapBlock(const std::vector<std::uint8_t>& bitmap,
                     std::uint32_t local_block, ondisk::Block* output) {
  const auto begin = bitmap.begin() +
                     static_cast<std::size_t>(local_block) *
                         ondisk::kBlockSize;
  std::copy_n(begin, ondisk::kBlockSize, output->begin());
}

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

}  // namespace

int PrepareFirstBlockWrite(const storage::ImageReader& image,
                           std::uint32_t inode_number,
                           std::string_view data,
                           std::uint64_t timestamp_ns,
                           FirstBlockWritePlan* output,
                           std::string* detail) {
  if (output == nullptr || data.empty()) {
    SetDetail(detail, "output and non-empty data are required");
    return -EINVAL;
  }
  if (data.size() > ondisk::kBlockSize) {
    SetDetail(detail, "first-block write cannot exceed one block");
    return -EFBIG;
  }
  if (detail != nullptr) {
    detail->clear();
  }

  ondisk::InodeRecord inode;
  int result = image.ReadInode(inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISREG(inode.mode)) {
    SetDetail(detail, "first-block write requires a regular file");
    return S_ISDIR(inode.mode) ? -EISDIR : -EINVAL;
  }
  if (inode.size != 0 || inode.indirect_block != 0 ||
      std::any_of(inode.direct_blocks.begin(), inode.direct_blocks.end(),
                  [](std::uint32_t block) { return block != 0; })) {
    SetDetail(detail, "first-block write currently requires an empty inode");
    return -EOPNOTSUPP;
  }

  const auto& superblock = image.superblock();
  std::vector<std::uint8_t> block_bitmap;
  result = LoadRegion(image, superblock.block_bitmap, &block_bitmap, detail);
  if (result != 0) {
    return result;
  }

  storage::BitmapAllocator block_allocator(
      &block_bitmap, superblock.total_blocks, superblock.data.start_block);
  std::string allocator_error;
  if (!block_allocator.Validate(&allocator_error)) {
    SetDetail(detail, allocator_error);
    return -EUCLEAN;
  }

  storage::BitmapReservation block_reservation;
  result = block_allocator.Reserve(&block_reservation, detail);
  if (result != 0) {
    return result;
  }

  FirstBlockWritePlan candidate;
  candidate.inode_number = inode_number;
  candidate.data_block = block_reservation.bit();
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;

  const std::uint32_t bitmap_local_block =
      candidate.data_block / (ondisk::kBlockSize * 8U);
  const std::uint32_t bitmap_home_block =
      superblock.block_bitmap.start_block + bitmap_local_block;
  result = image.ReadBlock(bitmap_home_block,
                           &candidate.before_images[bitmap_home_block], detail);
  if (result != 0) {
    return result;
  }
  CopyBitmapBlock(block_bitmap, bitmap_local_block,
                  &candidate.metadata_after_images[bitmap_home_block]);

  ondisk::Block data_before{};
  result = image.ReadBlock(candidate.data_block, &data_before, detail);
  if (result != 0) {
    return result;
  }
  candidate.before_images[candidate.data_block] = data_before;
  ondisk::Block data_after{};
  std::copy(data.begin(), data.end(), data_after.begin());
  candidate.ordered_data_after_images[candidate.data_block] = data_after;

  const std::uint32_t inode_table_block =
      InodeTableBlock(superblock, inode_number);
  ondisk::Block inode_table_before{};
  result = image.ReadBlock(inode_table_block, &inode_table_before, detail);
  if (result != 0) {
    return result;
  }
  candidate.before_images[inode_table_block] = inode_table_before;
  ondisk::Block inode_table_after = inode_table_before;

  inode.size = data.size();
  inode.direct_blocks[0] = candidate.data_block;
  inode.mtime_ns = timestamp_ns;
  inode.ctime_ns = timestamp_ns;
  ondisk::InodeBytes inode_bytes{};
  if (!ondisk::EncodeInode(inode, &inode_bytes, detail)) {
    return -EUCLEAN;
  }
  std::copy(inode_bytes.begin(), inode_bytes.end(),
            inode_table_after.begin() + InodeOffsetInBlock(inode_number));
  candidate.metadata_after_images[inode_table_block] = inode_table_after;

  block_reservation.KeepReserved();
  *output = std::move(candidate);
  return 0;
}

}  // namespace eufs::metadata
