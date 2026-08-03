#include "metadata/file_mutation_plan.h"

#include "storage/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
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

std::uint32_t RequiredBlocks(std::uint64_t size) {
  return size == 0
             ? 0
             : static_cast<std::uint32_t>(
                   (size - 1U) / ondisk::kBlockSize + 1U);
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

int LoadMapping(const storage::ImageReader& image,
                const ondisk::InodeRecord& inode,
                std::vector<std::uint32_t>* mapping,
                std::string* detail) {
  if (mapping == nullptr) {
    return -EINVAL;
  }
  const std::uint32_t count = RequiredBlocks(inode.size);
  mapping->assign(count, 0);
  for (std::uint32_t logical = 0; logical < count; ++logical) {
    const int result =
        image.MapLogicalBlock(inode, logical, &(*mapping)[logical], detail);
    if (result != 0) {
      return result;
    }
  }
  return 0;
}

int ReserveBlock(storage::BitmapAllocator* allocator,
                 std::vector<storage::BitmapReservation>* reservations,
                 std::uint32_t* block, std::string* detail) {
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
  if (overlap_begin < overlap_end) {
    std::fill(block->begin() +
                  static_cast<std::size_t>(overlap_begin - block_begin),
              block->begin() +
                  static_cast<std::size_t>(overlap_end - block_begin),
              0);
  }
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
  const std::size_t source =
      static_cast<std::size_t>(overlap_begin - write_begin);
  const std::size_t destination =
      static_cast<std::size_t>(overlap_begin - block_begin);
  const std::size_t count =
      static_cast<std::size_t>(overlap_end - overlap_begin);
  std::copy_n(data.begin() + source, count, block->begin() + destination);
}

int AddOrderedBlock(const storage::ImageReader& image,
                    storage::BitmapAllocator* allocator,
                    std::vector<storage::BitmapReservation>* reservations,
                    const ondisk::Block& after,
                    FileMutationPlan* plan,
                    std::uint32_t* physical,
                    std::string* detail) {
  int result = ReserveBlock(allocator, reservations, physical, detail);
  if (result != 0) {
    return result;
  }
  ondisk::Block before{};
  result = image.ReadBlock(*physical, &before, detail);
  if (result != 0) {
    return result;
  }
  plan->before_images[*physical] = before;
  plan->ordered_data_after_images[*physical] = after;
  return 0;
}

}  // 匿名命名空间：两种上层写语义共用这些磁盘转换步骤。

namespace detail {

int PrepareFileMutation(const storage::ImageReader& image,
                        std::uint32_t inode_number,
                        const FileMutationRequest& request,
                        FileMutationPlan* output,
                        std::string* detail) {
  if (output == nullptr || inode_number == 0) {
    SetDetail(detail, "file mutation requires inode and output");
    return -EINVAL;
  }
  if (request.mode == FileMutationMode::kWriteRange && request.data.empty()) {
    SetDetail(detail, "range write requires non-empty data");
    return -EINVAL;
  }
  if (request.offset > ondisk::kMaxFileSize ||
      request.data.size() > ondisk::kMaxFileSize - request.offset) {
    SetDetail(detail, "file mutation exceeds the v1 maximum file size");
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
    SetDetail(detail, "file mutation requires a regular file");
    return S_ISDIR(inode.mode) ? -EISDIR : -EINVAL;
  }
  if (request.mode == FileMutationMode::kReplaceAll &&
      inode.generation != request.expected_generation) {
    SetDetail(detail, "object version no longer matches expected generation");
    return -ESTALE;
  }
  // generation 是内容版本，任何成功内容变更都必须推进，禁止回绕。
  if (inode.generation == std::numeric_limits<std::uint64_t>::max()) {
    SetDetail(detail, "file generation cannot wrap");
    return -EOVERFLOW;
  }

  std::vector<std::uint32_t> old_mapping;
  result = LoadMapping(image, inode, &old_mapping, detail);
  if (result != 0) {
    return result;
  }
  std::set<std::uint32_t> old_owned(old_mapping.begin(), old_mapping.end());
  if (old_owned.size() != old_mapping.size() ||
      (inode.indirect_block != 0 &&
       !old_owned.insert(inode.indirect_block).second)) {
    SetDetail(detail, "file mapping contains duplicate owned blocks");
    return -EUCLEAN;
  }

  const auto& superblock = image.superblock();
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

  const bool replace_all = request.mode == FileMutationMode::kReplaceAll;
  const std::uint64_t write_end = request.offset + request.data.size();
  const std::uint64_t new_size =
      replace_all ? request.data.size() : std::max(inode.size, write_end);
  const std::uint32_t old_count = RequiredBlocks(inode.size);
  const std::uint32_t new_count = RequiredBlocks(new_size);

  FileMutationPlan candidate;
  candidate.inode_number = inode_number;
  candidate.old_generation = inode.generation;
  candidate.new_generation = inode.generation + 1U;
  candidate.old_size = inode.size;
  candidate.new_size = new_size;
  candidate.old_data_blocks = old_mapping;
  candidate.old_indirect_block = inode.indirect_block;
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;

  std::vector<std::uint32_t> new_mapping =
      replace_all ? std::vector<std::uint32_t>(new_count, 0) : old_mapping;
  new_mapping.resize(new_count, 0);
  std::vector<storage::BitmapReservation> reservations;
  std::set<std::uint32_t> released;
  std::uint32_t last_changed = 0;

  if (replace_all) {
    for (std::uint32_t logical = 0; logical < new_count; ++logical) {
      ondisk::Block after{};
      const std::size_t source =
          static_cast<std::size_t>(logical) * ondisk::kBlockSize;
      const std::size_t count = std::min<std::size_t>(
          ondisk::kBlockSize, request.data.size() - source);
      std::copy_n(request.data.begin() + source, count, after.begin());
      result = AddOrderedBlock(image, &allocator, &reservations, after,
                               &candidate, &new_mapping[logical], detail);
      if (result != 0) {
        return result;
      }
    }
    released.insert(old_mapping.begin(), old_mapping.end());
    last_changed = new_count == 0 ? 0 : new_count - 1U;
  } else {
    const std::uint64_t changed_begin = std::min(inode.size, request.offset);
    const std::uint32_t first_changed =
        static_cast<std::uint32_t>(changed_begin / ondisk::kBlockSize);
    last_changed =
        static_cast<std::uint32_t>((write_end - 1U) / ondisk::kBlockSize);
    for (std::uint32_t logical = first_changed; logical <= last_changed;
         ++logical) {
      ondisk::Block after{};
      const std::uint64_t block_begin =
          static_cast<std::uint64_t>(logical) * ondisk::kBlockSize;
      const bool covers_whole =
          request.offset <= block_begin &&
          write_end >= block_begin + ondisk::kBlockSize;
      if (logical < old_count) {
        const std::uint32_t old_physical = old_mapping[logical];
        if (!covers_whole) {
          result = image.ReadBlock(old_physical, &after, detail);
          if (result != 0) {
            return result;
          }
        }
        released.insert(old_physical);
      }
      if (request.offset > inode.size) {
        ZeroRange(inode.size, request.offset, block_begin, &after);
      }
      OverlayData(request.offset, write_end, block_begin, request.data, &after);
      result = AddOrderedBlock(image, &allocator, &reservations, after,
                               &candidate, &new_mapping[logical], detail);
      if (result != 0) {
        return result;
      }
    }
  }

  const bool changes_indirect =
      new_count > ondisk::kDirectBlockCount &&
      (replace_all || inode.indirect_block == 0 ||
       last_changed >= ondisk::kDirectBlockCount);
  std::uint32_t new_indirect =
      replace_all ? 0 : inode.indirect_block;
  if (changes_indirect) {
    result = ReserveBlock(&allocator, &reservations, &new_indirect, detail);
    if (result != 0) {
      return result;
    }
  }
  if (inode.indirect_block != 0 &&
      (replace_all || changes_indirect)) {
    released.insert(inode.indirect_block);
  }

  for (const std::uint32_t block : released) {
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
      const std::uint32_t home = superblock.block_bitmap.start_block + local;
      candidate.before_images[home] = before;
      candidate.metadata_after_images[home] = after;
    }
  }

  if (changes_indirect) {
    ondisk::Block indirect_before{};
    result = image.ReadBlock(new_indirect, &indirect_before, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block indirect_after{};
    for (std::uint32_t logical = ondisk::kDirectBlockCount;
         logical < new_count; ++logical) {
      const std::uint32_t index =
          logical - static_cast<std::uint32_t>(ondisk::kDirectBlockCount);
      PutLe32(indirect_after.data() + index * sizeof(std::uint32_t),
              new_mapping[logical]);
    }
    candidate.before_images[new_indirect] = indirect_before;
    candidate.metadata_after_images[new_indirect] = indirect_after;
  }

  const std::uint32_t inode_table =
      InodeTableBlock(superblock, inode_number);
  ondisk::Block inode_before{};
  result = image.ReadBlock(inode_table, &inode_before, detail);
  if (result != 0) {
    return result;
  }
  ondisk::Block inode_after = inode_before;
  inode.size = new_size;
  inode.mtime_ns = request.timestamp_ns;
  inode.ctime_ns = request.timestamp_ns;
  inode.generation = candidate.new_generation;
  inode.direct_blocks.fill(0);
  const std::uint32_t direct_count = std::min<std::uint32_t>(
      new_count, static_cast<std::uint32_t>(ondisk::kDirectBlockCount));
  for (std::uint32_t logical = 0; logical < direct_count; ++logical) {
    inode.direct_blocks[logical] = new_mapping[logical];
  }
  inode.indirect_block =
      new_count > ondisk::kDirectBlockCount ? new_indirect : 0;
  ondisk::InodeBytes inode_bytes{};
  if (!ondisk::EncodeInode(inode, &inode_bytes, detail)) {
    return -EUCLEAN;
  }
  std::copy(inode_bytes.begin(), inode_bytes.end(),
            inode_after.begin() + InodeOffsetInBlock(inode_number));
  candidate.before_images[inode_table] = inode_before;
  candidate.metadata_after_images[inode_table] = inode_after;

  candidate.new_data_blocks = new_mapping;
  candidate.new_indirect_block = inode.indirect_block;
  for (auto& reservation : reservations) {
    reservation.KeepReserved();
  }
  *output = std::move(candidate);
  return 0;
}

}  // namespace detail

}  // namespace eufs::metadata
