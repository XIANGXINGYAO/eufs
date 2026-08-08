#include "metadata/new_object_plan.h"

#include "storage/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

namespace eufs::metadata {

// Backend 根对象名校验：禁止空名、`.`、`..`、斜杠、NUL 和超长名称。
bool IsValidRootObjectName(std::string_view name) {
  return !name.empty() && name != "." && name != ".." &&
         name.size() <= ondisk::kMaxNameLength &&
         name.find('/') == std::string_view::npos &&
         name.find('\0') == std::string_view::npos;
}

namespace {

// 本文件把完整对象内容、inode、目录项、bitmap 和间接块联合成一次原子发布计划。
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

// 计算完整对象需要的数据块数。
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

// 从 allocator 预留一个块，并保存 reservation 以便失败自动回滚。
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

// 首次编辑 metadata home block 时读取 before-image，后续复用同一 after-image。
int MutableMetadataBlock(const storage::ImageReader& image,
                         std::uint32_t block_number, NewObjectPlan* plan,
                         ondisk::Block** output, std::string* detail) {
  auto [entry, inserted] =
      plan->metadata_after_images.try_emplace(block_number);
  if (inserted) {
    ondisk::Block before{};
    const int result = image.ReadBlock(block_number, &before, detail);
    if (result != 0) {
      plan->metadata_after_images.erase(entry);
      return result;
    }
    plan->before_images[block_number] = before;
    entry->second = before;
  }
  *output = &entry->second;
  return 0;
}

// 尝试在现有目录块中分裂空闲 record_length 并插入新目录项。
bool TryInsertDirectoryEntry(const ondisk::DirectoryEntry& new_entry,
                             ondisk::Block* block, std::string* detail) {
  const std::size_t new_minimum =
      ondisk::MinimumDirectoryRecordLength(new_entry.name.size());
  std::size_t offset = 0;
  while (offset < block->size()) {
    ondisk::DirectoryEntry current;
    if (!ondisk::DecodeDirectoryEntry(block->data() + offset,
                                      block->size() - offset, &current,
                                      detail) ||
        current.record_length == 0) {
      return false;
    }
    const std::size_t record_length = current.record_length;
    if (current.inode == 0) {
      if (record_length >= new_minimum) {
        return ondisk::EncodeDirectoryEntry(
            new_entry, static_cast<std::uint16_t>(record_length),
            block->data() + offset, block->size() - offset, detail);
      }
    } else {
      const std::size_t current_minimum =
          ondisk::MinimumDirectoryRecordLength(current.name.size());
      if (record_length - current_minimum >= new_minimum) {
        if (!ondisk::EncodeDirectoryEntry(
                current, static_cast<std::uint16_t>(current_minimum),
                block->data() + offset, block->size() - offset, detail)) {
          return false;
        }
        return ondisk::EncodeDirectoryEntry(
            new_entry,
            static_cast<std::uint16_t>(record_length - current_minimum),
            block->data() + offset + current_minimum,
            block->size() - offset - current_minimum, detail);
      }
    }
    offset += record_length;
  }
  return false;
}

// 比较 bitmap before/after，只把真正变化的 bitmap 块加入 metadata 事务。
int AddBitmapAfterImages(const storage::ImageReader& image,
                         const ondisk::Region& region,
                         const std::vector<std::uint8_t>& before,
                         const std::vector<std::uint8_t>& after,
                         NewObjectPlan* plan, std::string* detail) {
  for (std::uint32_t local = 0; local < region.block_count; ++local) {
    const ondisk::Block before_block = BitmapBlock(before, local);
    const ondisk::Block after_block = BitmapBlock(after, local);
    if (before_block == after_block) {
      continue;
    }
    const std::uint32_t home = region.start_block + local;
    ondisk::Block physical_before{};
    const int result = image.ReadBlock(home, &physical_before, detail);
    if (result != 0) {
      return result;
    }
    if (physical_before != before_block) {
      SetDetail(detail, "bitmap changed while building new-object plan");
      return -ESTALE;
    }
    plan->before_images[home] = before_block;
    plan->metadata_after_images[home] = after_block;
  }
  return 0;
}

}  // 匿名命名空间。

// 原子规划根目录新对象；目录无空间时可增长 direct/single-indirect 映射。
int PrepareNewRootObject(const storage::ImageReader& image,
                         std::string_view name, std::string_view data,
                         std::uint32_t permissions, std::uint32_t uid,
                         std::uint32_t gid, std::uint64_t timestamp_ns,
                         NewObjectPlan* output, std::string* detail) {
  if (output == nullptr || !IsValidRootObjectName(name)) {
    SetDetail(detail, "new object requires an output and valid root name");
    return -EINVAL;
  }
  if (data.size() > ondisk::kMaxFileSize) {
    SetDetail(detail, "new object exceeds the v1 maximum file size");
    return -EFBIG;
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

  std::vector<ondisk::DirectoryEntry> existing_entries;
  result = image.ListDirectory(superblock.root_inode, &existing_entries,
                               detail);
  if (result != 0) {
    return result;
  }
  if (std::any_of(existing_entries.begin(), existing_entries.end(),
                  [name](const auto& entry) { return entry.name == name; })) {
    SetDetail(detail, "root object name already exists");
    return -EEXIST;
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
  const auto inode_bitmap_before = inode_bitmap;
  const auto block_bitmap_before = block_bitmap;

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
  result = inode_allocator.Reserve(&inode_reservation, detail);
  if (result != 0) {
    return result;
  }
  std::vector<storage::BitmapReservation> block_reservations;

  NewObjectPlan candidate;
  candidate.inode_number = inode_reservation.bit() + 1U;
  candidate.object_size = data.size();
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;

  ondisk::DirectoryEntry new_entry;
  new_entry.inode = candidate.inode_number;
  new_entry.file_type = ondisk::DirectoryFileType::kRegular;
  new_entry.name.assign(name);

  const std::uint32_t root_block_count =
      static_cast<std::uint32_t>(root.size / ondisk::kBlockSize);
  for (std::uint32_t logical = 0; logical < root_block_count; ++logical) {
    std::uint32_t physical = 0;
    result = image.MapLogicalBlock(root, logical, &physical, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block directory_after{};
    result = image.ReadBlock(physical, &directory_after, detail);
    if (result != 0) {
      return result;
    }
    const ondisk::Block directory_before = directory_after;
    std::string insertion_detail;
    if (TryInsertDirectoryEntry(new_entry, &directory_after,
                                &insertion_detail)) {
      candidate.directory_block = physical;
      candidate.before_images[physical] = directory_before;
      candidate.metadata_after_images[physical] = directory_after;
      break;
    }
    if (!insertion_detail.empty()) {
      SetDetail(detail, insertion_detail);
      return -EUCLEAN;
    }
  }

  std::uint32_t old_root_indirect = root.indirect_block;
  if (candidate.directory_block == 0) {
    const std::uint32_t max_blocks = static_cast<std::uint32_t>(
        ondisk::kMaxFileSize / ondisk::kBlockSize);
    if (root_block_count >= max_blocks) {
      SetDetail(detail, "root directory reached the v1 mapping limit");
      return -ENOSPC;
    }
    result = ReserveBlock(&block_allocator, &block_reservations,
                          &candidate.directory_block, detail);
    if (result != 0) {
      return result;
    }
    candidate.directory_grew = true;
    ondisk::Block* directory_after = nullptr;
    result = MutableMetadataBlock(image, candidate.directory_block,
                                  &candidate, &directory_after, detail);
    if (result != 0) {
      return result;
    }
    directory_after->fill(0);
    if (!ondisk::EncodeDirectoryEntry(
            new_entry, static_cast<std::uint16_t>(ondisk::kBlockSize),
            directory_after->data(), directory_after->size(), detail)) {
      return -EINVAL;
    }

    if (root_block_count < ondisk::kDirectBlockCount) {
      root.direct_blocks[root_block_count] = candidate.directory_block;
    } else {
      result = ReserveBlock(&block_allocator, &block_reservations,
                            &candidate.root_indirect_block, detail);
      if (result != 0) {
        return result;
      }
      ondisk::Block* indirect_after = nullptr;
      result = MutableMetadataBlock(image, candidate.root_indirect_block,
                                    &candidate, &indirect_after, detail);
      if (result != 0) {
        return result;
      }
      indirect_after->fill(0);
      if (old_root_indirect != 0) {
        result = image.ReadBlock(old_root_indirect, indirect_after, detail);
        if (result != 0) {
          return result;
        }
      }
      const std::uint32_t indirect_index =
          root_block_count -
          static_cast<std::uint32_t>(ondisk::kDirectBlockCount);
      PutLe32(indirect_after->data() +
                  static_cast<std::size_t>(indirect_index) *
                      sizeof(std::uint32_t),
              candidate.directory_block);
      root.indirect_block = candidate.root_indirect_block;
    }
    root.size += ondisk::kBlockSize;
  }

  const std::uint32_t file_block_count = RequiredBlocks(data.size());
  candidate.data_blocks.reserve(file_block_count);
  for (std::uint32_t logical = 0; logical < file_block_count; ++logical) {
    std::uint32_t physical = 0;
    result = ReserveBlock(&block_allocator, &block_reservations, &physical,
                          detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block before{};
    result = image.ReadBlock(physical, &before, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block after{};
    const std::size_t source_offset =
        static_cast<std::size_t>(logical) * ondisk::kBlockSize;
    const std::size_t count = std::min<std::size_t>(
        ondisk::kBlockSize, data.size() - source_offset);
    std::copy_n(data.begin() + source_offset, count, after.begin());
    candidate.before_images[physical] = before;
    candidate.ordered_data_after_images[physical] = after;
    candidate.data_blocks.push_back(physical);
  }

  if (file_block_count > ondisk::kDirectBlockCount) {
    result = ReserveBlock(&block_allocator, &block_reservations,
                          &candidate.file_indirect_block, detail);
    if (result != 0) {
      return result;
    }
    ondisk::Block* indirect_after = nullptr;
    result = MutableMetadataBlock(image, candidate.file_indirect_block,
                                  &candidate, &indirect_after, detail);
    if (result != 0) {
      return result;
    }
    indirect_after->fill(0);
    for (std::uint32_t logical = ondisk::kDirectBlockCount;
         logical < file_block_count; ++logical) {
      const std::uint32_t index =
          logical - static_cast<std::uint32_t>(ondisk::kDirectBlockCount);
      PutLe32(indirect_after->data() +
                  static_cast<std::size_t>(index) * sizeof(std::uint32_t),
              candidate.data_blocks[logical]);
    }
  }

  if (candidate.root_indirect_block != 0 && old_root_indirect != 0) {
    result = block_allocator.ReleaseAllocated(old_root_indirect, detail);
    if (result != 0) {
      return result;
    }
  }

  ondisk::InodeRecord file;
  file.inode_number = candidate.inode_number;
  file.mode = S_IFREG | (permissions & 0777U);
  file.uid = uid;
  file.gid = gid;
  file.link_count = 1;
  file.size = data.size();
  file.atime_ns = timestamp_ns;
  file.mtime_ns = timestamp_ns;
  file.ctime_ns = timestamp_ns;
  file.generation = 1;
  const std::uint32_t direct_count = std::min<std::uint32_t>(
      file_block_count, static_cast<std::uint32_t>(ondisk::kDirectBlockCount));
  for (std::uint32_t logical = 0; logical < direct_count; ++logical) {
    file.direct_blocks[logical] = candidate.data_blocks[logical];
  }
  file.indirect_block = candidate.file_indirect_block;

  root.mtime_ns = timestamp_ns;
  root.ctime_ns = timestamp_ns;
  const std::uint32_t root_table =
      InodeTableBlock(superblock, superblock.root_inode);
  ondisk::Block* root_table_after = nullptr;
  result = MutableMetadataBlock(image, root_table, &candidate,
                                &root_table_after, detail);
  if (result != 0) {
    return result;
  }
  ondisk::InodeBytes root_bytes{};
  if (!ondisk::EncodeInode(root, &root_bytes, detail)) {
    return -EUCLEAN;
  }
  std::copy(root_bytes.begin(), root_bytes.end(),
            root_table_after->begin() +
                InodeOffsetInBlock(superblock.root_inode));

  const std::uint32_t file_table =
      InodeTableBlock(superblock, candidate.inode_number);
  ondisk::Block* file_table_after = nullptr;
  result = MutableMetadataBlock(image, file_table, &candidate,
                                &file_table_after, detail);
  if (result != 0) {
    return result;
  }
  ondisk::InodeBytes file_bytes{};
  if (!ondisk::EncodeInode(file, &file_bytes, detail)) {
    return -EUCLEAN;
  }
  std::copy(file_bytes.begin(), file_bytes.end(),
            file_table_after->begin() +
                InodeOffsetInBlock(candidate.inode_number));

  result = AddBitmapAfterImages(
      image, superblock.inode_bitmap, inode_bitmap_before,
      inode_allocator.Snapshot(), &candidate, detail);
  if (result == 0) {
    result = AddBitmapAfterImages(
        image, superblock.block_bitmap, block_bitmap_before,
        block_allocator.Snapshot(), &candidate, detail);
  }
  if (result != 0) {
    return result;
  }

  inode_reservation.KeepReserved();
  for (auto& reservation : block_reservations) {
    reservation.KeepReserved();
  }
  *output = std::move(candidate);
  return 0;
}

}  // namespace eufs::metadata
  // 先检查名称、权限、大小和输出契约，再查重。
  // 新 inode、所有数据块和必要间接块先在 bitmap 内存副本中预留。
  // payload 被拆成完整 COW 数据块，块内未使用尾部保持 0。
  // 优先插入现有目录块；全部装不下时为根目录分配新数据块。
  // inode/目录/bitmap/间接块是 metadata after-image，随唯一 COMMIT 一起发布。
  // candidate 全部完成后才 KeepReserved 并写入 output。
