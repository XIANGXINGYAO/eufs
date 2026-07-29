#include "metadata/file_write_plan.h"

#include "storage/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace eufs::metadata {
namespace {

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void PutLe32(std::uint8_t* output, std::uint32_t value) {
  output[0] = static_cast<std::uint8_t>(value & 0xFFU);
  output[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
  output[2] = static_cast<std::uint8_t>((value >> 16U) & 0xFFU);
  output[3] = static_cast<std::uint8_t>((value >> 24U) & 0xFFU);
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  return static_cast<std::uint32_t>(input[0]) |
         (static_cast<std::uint32_t>(input[1]) << 8U) |
         (static_cast<std::uint32_t>(input[2]) << 16U) |
         (static_cast<std::uint32_t>(input[3]) << 24U);
}

int LoadRegion(const storage::ImageReader& image, const ondisk::Region& region,
               std::vector<std::uint8_t>* output, std::string* detail) {
  if (output == nullptr) {
    return -EINVAL;
  }
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

ondisk::Block BitmapBlock(const std::vector<std::uint8_t>& bitmap,
                          std::uint32_t local_block) {
  ondisk::Block output{};
  const auto begin = bitmap.begin() +
                     static_cast<std::size_t>(local_block) *
                         ondisk::kBlockSize;
  std::copy_n(begin, ondisk::kBlockSize, output.begin());
  return output;
}

std::uint32_t RequiredBlocks(std::uint64_t size) {
  return size == 0
             ? 0
             : static_cast<std::uint32_t>(
                   (size - 1U) / ondisk::kBlockSize + 1U);
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

int LoadOldMapping(const storage::ImageReader& image,
                   const ondisk::InodeRecord& inode,
                   std::vector<std::uint32_t>* mapping,
                   ondisk::Block* indirect_before, std::string* detail) {
  if (mapping == nullptr || indirect_before == nullptr) {
    return -EINVAL;
  }
  const std::uint32_t block_count = RequiredBlocks(inode.size);
  mapping->assign(block_count, 0);
  const std::uint32_t direct_count = std::min<std::uint32_t>(
      block_count, static_cast<std::uint32_t>(ondisk::kDirectBlockCount));
  for (std::uint32_t logical = 0; logical < direct_count; ++logical) {
    (*mapping)[logical] = inode.direct_blocks[logical];
  }
  indirect_before->fill(0);
  if (block_count <= ondisk::kDirectBlockCount) {
    return 0;
  }
  int result = image.ReadBlock(inode.indirect_block, indirect_before, detail);
  if (result != 0) {
    return result;
  }
  for (std::uint32_t logical = ondisk::kDirectBlockCount;
       logical < block_count; ++logical) {
    const std::uint32_t indirect_index =
        logical - static_cast<std::uint32_t>(ondisk::kDirectBlockCount);
    (*mapping)[logical] = GetLe32(
        indirect_before->data() + indirect_index * sizeof(std::uint32_t));
  }
  return 0;
}

int ReserveBlock(storage::BitmapAllocator* allocator,
                 std::vector<storage::BitmapReservation>* reservations,
                 std::uint32_t* block, std::string* detail) {
  if (allocator == nullptr || reservations == nullptr || block == nullptr) {
    return -EINVAL;
  }
  storage::BitmapReservation reservation;
  const int result = allocator->Reserve(&reservation, detail);
  if (result != 0) {
    return result;
  }
  *block = reservation.bit();
  reservations->push_back(std::move(reservation));
  return 0;
}

void ZeroRange(std::uint64_t begin, std::uint64_t end,
               std::uint64_t block_begin, ondisk::Block* block) {
  const std::uint64_t overlap_begin = std::max(begin, block_begin);
  const std::uint64_t overlap_end =
      std::min(end, block_begin + ondisk::kBlockSize);
  if (overlap_begin >= overlap_end) {
    return;
  }
  std::fill(block->begin() + static_cast<std::size_t>(overlap_begin - block_begin),
            block->begin() + static_cast<std::size_t>(overlap_end - block_begin),
            0);
}

void OverlayData(std::uint64_t write_begin, std::uint64_t write_end,
                 std::uint64_t block_begin, std::string_view data,
                 ondisk::Block* block) {
  const std::uint64_t overlap_begin = std::max(write_begin, block_begin);
  const std::uint64_t overlap_end =
      std::min(write_end, block_begin + ondisk::kBlockSize);
  if (overlap_begin >= overlap_end) {
    return;
  }
  const std::size_t source_offset =
      static_cast<std::size_t>(overlap_begin - write_begin);
  const std::size_t destination_offset =
      static_cast<std::size_t>(overlap_begin - block_begin);
  const std::size_t count =
      static_cast<std::size_t>(overlap_end - overlap_begin);
  std::copy_n(data.begin() + source_offset, count,
              block->begin() + destination_offset);
}

}  // namespace

int PrepareFileWrite(const storage::ImageReader& image,
                     std::uint32_t inode_number, std::uint64_t offset,
                     std::string_view data, std::uint64_t timestamp_ns,
                     FileWritePlan* output, std::string* detail) {
  if (output == nullptr || data.empty()) {
    SetDetail(detail, "output and non-empty write data are required");
    return -EINVAL;
  }
  if (offset > ondisk::kMaxFileSize ||
      data.size() > ondisk::kMaxFileSize - offset) {
    SetDetail(detail, "write exceeds the v1 maximum file size");
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
    SetDetail(detail, "file write requires a regular file");
    return S_ISDIR(inode.mode) ? -EISDIR : -EINVAL;
  }

  const auto& superblock = image.superblock();
  const std::uint64_t write_end = offset + data.size();
  const std::uint64_t new_size = std::max(inode.size, write_end);
  const std::uint32_t old_block_count = RequiredBlocks(inode.size);
  const std::uint32_t new_block_count = RequiredBlocks(new_size);

  ondisk::Block old_indirect{};
  std::vector<std::uint32_t> old_mapping;
  result = LoadOldMapping(image, inode, &old_mapping, &old_indirect, detail);
  if (result != 0) {
    return result;
  }
  std::set<std::uint32_t> old_owned_blocks(old_mapping.begin(),
                                           old_mapping.end());
  if (old_owned_blocks.size() != old_mapping.size() ||
      (inode.indirect_block != 0 &&
       !old_owned_blocks.insert(inode.indirect_block).second)) {
    SetDetail(detail, "file mapping contains a duplicate owned block");
    return -EUCLEAN;
  }
  std::vector<std::uint32_t> new_mapping = old_mapping;
  new_mapping.resize(new_block_count, 0);

  std::vector<std::uint8_t> block_bitmap;
  result = LoadRegion(image, superblock.block_bitmap, &block_bitmap, detail);
  if (result != 0) {
    return result;
  }
  const std::vector<std::uint8_t> block_bitmap_before = block_bitmap;
  storage::BitmapAllocator allocator(
      &block_bitmap, superblock.total_blocks, superblock.data.start_block);
  std::string allocator_error;
  if (!allocator.Validate(&allocator_error)) {
    SetDetail(detail, allocator_error);
    return -EUCLEAN;
  }

  FileWritePlan candidate;
  candidate.inode_number = inode_number;
  candidate.old_size = inode.size;
  candidate.new_size = new_size;
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;

  std::vector<storage::BitmapReservation> reservations;
  std::set<std::uint32_t> released_blocks;
  const std::uint64_t changed_begin = std::min(inode.size, offset);
  const std::uint32_t first_changed =
      static_cast<std::uint32_t>(changed_begin / ondisk::kBlockSize);
  const std::uint32_t last_changed =
      static_cast<std::uint32_t>((write_end - 1U) / ondisk::kBlockSize);

  for (std::uint32_t logical = first_changed; logical <= last_changed;
       ++logical) {
    std::uint32_t new_physical = 0;
    result = ReserveBlock(&allocator, &reservations, &new_physical, detail);
    if (result != 0) {
      return result;
    }

    ondisk::Block after{};
    const std::uint64_t block_begin =
        static_cast<std::uint64_t>(logical) * ondisk::kBlockSize;
    const bool write_covers_whole_block =
        offset <= block_begin &&
        write_end >= block_begin + ondisk::kBlockSize;
    if (logical < old_block_count) {
      const std::uint32_t old_physical = old_mapping[logical];
      if (!write_covers_whole_block) {
        result = image.ReadBlock(old_physical, &after, detail);
        if (result != 0) {
          return result;
        }
      }
      released_blocks.insert(old_physical);
    }
    if (offset > inode.size) {
      ZeroRange(inode.size, offset, block_begin, &after);
    }
    OverlayData(offset, write_end, block_begin, data, &after);

    ondisk::Block data_before{};
    result = image.ReadBlock(new_physical, &data_before, detail);
    if (result != 0) {
      return result;
    }
    candidate.before_images[new_physical] = data_before;
    candidate.ordered_data_after_images[new_physical] = after;
    new_mapping[logical] = new_physical;
  }

  const bool changes_indirect =
      new_block_count > ondisk::kDirectBlockCount &&
      (inode.indirect_block == 0 ||
       last_changed >= ondisk::kDirectBlockCount);
  std::uint32_t new_indirect_block = inode.indirect_block;
  if (changes_indirect) {
    result = ReserveBlock(&allocator, &reservations, &new_indirect_block,
                          detail);
    if (result != 0) {
      return result;
    }
    if (inode.indirect_block != 0) {
      released_blocks.insert(inode.indirect_block);
    }
  }

  for (const std::uint32_t block : released_blocks) {
    result = allocator.ReleaseAllocated(block, detail);
    if (result != 0) {
      return result;
    }
  }

  const std::vector<std::uint8_t> block_bitmap_after = allocator.Snapshot();
  for (std::uint32_t local = 0;
       local < superblock.block_bitmap.block_count; ++local) {
    const ondisk::Block before = BitmapBlock(block_bitmap_before, local);
    const ondisk::Block after = BitmapBlock(block_bitmap_after, local);
    if (before != after) {
      const std::uint32_t home =
          superblock.block_bitmap.start_block + local;
      candidate.before_images[home] = before;
      candidate.metadata_after_images[home] = after;
    }
  }

  if (changes_indirect) {
    ondisk::Block indirect_before{};
    result = image.ReadBlock(new_indirect_block, &indirect_before, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block indirect_after{};
    for (std::uint32_t logical = ondisk::kDirectBlockCount;
         logical < new_block_count; ++logical) {
      const std::uint32_t index =
          logical - static_cast<std::uint32_t>(ondisk::kDirectBlockCount);
      PutLe32(indirect_after.data() + index * sizeof(std::uint32_t),
              new_mapping[logical]);
    }
    candidate.before_images[new_indirect_block] = indirect_before;
    candidate.metadata_after_images[new_indirect_block] = indirect_after;
  }

  const std::uint32_t inode_table_block =
      InodeTableBlock(superblock, inode_number);
  ondisk::Block inode_table_before{};
  result = image.ReadBlock(inode_table_block, &inode_table_before, detail);
  if (result != 0) {
    return result;
  }
  ondisk::Block inode_table_after = inode_table_before;
  inode.size = new_size;
  inode.mtime_ns = timestamp_ns;
  inode.ctime_ns = timestamp_ns;
  inode.direct_blocks.fill(0);
  const std::uint32_t direct_count = std::min<std::uint32_t>(
      new_block_count, static_cast<std::uint32_t>(ondisk::kDirectBlockCount));
  for (std::uint32_t logical = 0; logical < direct_count; ++logical) {
    inode.direct_blocks[logical] = new_mapping[logical];
  }
  inode.indirect_block =
      new_block_count > ondisk::kDirectBlockCount ? new_indirect_block : 0;
  ondisk::InodeBytes inode_bytes{};
  if (!ondisk::EncodeInode(inode, &inode_bytes, detail)) {
    return -EUCLEAN;
  }
  std::copy(inode_bytes.begin(), inode_bytes.end(),
            inode_table_after.begin() + InodeOffsetInBlock(inode_number));
  candidate.before_images[inode_table_block] = inode_table_before;
  candidate.metadata_after_images[inode_table_block] = inode_table_after;

  for (auto& reservation : reservations) {
    reservation.KeepReserved();
  }
  *output = std::move(candidate);
  return 0;
}

}  // namespace eufs::metadata
