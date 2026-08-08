#include "tests/support/first_block_write_plan.h"

// 该单块计划器只用于构造分配器、日志和恢复测试场景。
// 它只能向空 inode 写入一个数据块，不属于当前 eufsd 的通用写路径。
// 当前挂载写入使用 src/metadata/file_write_plan.cpp 中的 PrepareFileWrite。

#include "storage/bitmap_allocator.h"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <string_view>
#include <sys/stat.h>
#include <vector>

namespace eufs::metadata {
namespace {

// detail 是可选的人类可读错误输出。
void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

// 把一个连续磁盘区域完整读入字节数组，供 bitmap allocator 原地修改。
int LoadRegion(const storage::ImageReader& image, const ondisk::Region& region,
               std::vector<std::uint8_t>* output, std::string* detail) {
  output->assign(static_cast<std::size_t>(region.block_count) *
                     ondisk::kBlockSize,
                 0);
  // ImageReader 以 4 KiB 块为接口，因此逐块读出再拼到连续数组。
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

// allocator 修改的是完整 bitmap 数组；这里只抽出发生变化的那个 home block。
void CopyBitmapBlock(const std::vector<std::uint8_t>& bitmap,
                     std::uint32_t local_block, ondisk::Block* output) {
  const auto begin = bitmap.begin() +
                     static_cast<std::size_t>(local_block) *
                         ondisk::kBlockSize;
  std::copy_n(begin, ondisk::kBlockSize, output->begin());
}

// 根据固定 128 字节 inode 记录计算它位于 inode table 的哪一个物理块。
std::uint32_t InodeTableBlock(const ondisk::Superblock& superblock,
                              std::uint32_t inode_number) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(inode_number - 1U) *
      ondisk::kInodeRecordSize;
  return superblock.inode_table.start_block +
         static_cast<std::uint32_t>(byte_index / ondisk::kBlockSize);
}

// 计算 inode 记录在对应 4 KiB inode-table 块内部的起始偏移。
std::size_t InodeOffsetInBlock(std::uint32_t inode_number) {
  const std::uint64_t byte_index =
      static_cast<std::uint64_t>(inode_number - 1U) *
      ondisk::kInodeRecordSize;
  return static_cast<std::size_t>(byte_index % ondisk::kBlockSize);
}

}  // 匿名命名空间。

// 为“空普通文件第一次写入”生成 ordered-data WAL 所需的 before/after 块集合。
int PrepareFirstBlockWrite(const storage::ImageReader& image,
                           std::uint32_t inode_number,
                           std::string_view data,
                           std::uint64_t timestamp_ns,
                           FirstBlockWritePlan* output,
                           std::string* detail) {
  // 空数据没有测试价值；超过一块则超出这个历史 fixture 的明确能力边界。
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

  // 先验证目标 inode 当前确实存在且是普通文件。
  ondisk::InodeRecord inode;
  int result = image.ReadInode(inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISREG(inode.mode)) {
    SetDetail(detail, "first-block write requires a regular file");
    return S_ISDIR(inode.mode) ? -EISDIR : -EINVAL;
  }
  // 该测试计划绝不处理覆盖、追加或已有间接块，避免与生产通用 planner 重叠。
  if (inode.size != 0 || inode.indirect_block != 0 ||
      std::any_of(inode.direct_blocks.begin(), inode.direct_blocks.end(),
                  [](std::uint32_t block) { return block != 0; })) {
    SetDetail(detail, "first-block write currently requires an empty inode");
    return -EOPNOTSUPP;
  }

  // 读取完整 block bitmap，随后在内存副本上预留一个数据块。
  const auto& superblock = image.superblock();
  std::vector<std::uint8_t> block_bitmap;
  result = LoadRegion(image, superblock.block_bitmap, &block_bitmap, detail);
  if (result != 0) {
    return result;
  }

  // allocator 校验 bitmap 几何和保留前缀，损坏镜像不能继续生成计划。
  storage::BitmapAllocator block_allocator(
      &block_bitmap, superblock.total_blocks, superblock.data.start_block);
  std::string allocator_error;
  if (!block_allocator.Validate(&allocator_error)) {
    SetDetail(detail, allocator_error);
    return -EUCLEAN;
  }

  // reservation 在函数异常返回时会自动回滚内存位，成功末尾才正式保留。
  storage::BitmapReservation block_reservation;
  result = block_allocator.Reserve(&block_reservation, detail);
  if (result != 0) {
    return result;
  }

  // candidate 在全部步骤成功前保持局部，调用者不会看到半成品计划。
  FirstBlockWritePlan candidate;
  candidate.inode_number = inode_number;
  candidate.data_block = block_reservation.bit();
  candidate.total_blocks = superblock.total_blocks;
  candidate.filesystem_uuid = superblock.filesystem_uuid;

  // bitmap 所在 home block 同时记录旧镜像和置位后的新镜像。
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

  // 新数据块归入 ordered_data_after_images，要求先于日志 COMMIT 持久化。
  ondisk::Block data_before{};
  result = image.ReadBlock(candidate.data_block, &data_before, detail);
  if (result != 0) {
    return result;
  }
  candidate.before_images[candidate.data_block] = data_before;
  ondisk::Block data_after{};
  std::copy(data.begin(), data.end(), data_after.begin());
  candidate.ordered_data_after_images[candidate.data_block] = data_after;

  // inode table 是元数据：以旧块为底，只替换目标 inode 的 128 字节记录。
  const std::uint32_t inode_table_block =
      InodeTableBlock(superblock, inode_number);
  ondisk::Block inode_table_before{};
  result = image.ReadBlock(inode_table_block, &inode_table_before, detail);
  if (result != 0) {
    return result;
  }
  candidate.before_images[inode_table_block] = inode_table_before;
  ondisk::Block inode_table_after = inode_table_before;

  // inode 只更新大小、第一直接块和时间戳，目录项在文件创建阶段已经存在。
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

  // 计划已经完整接管该位；阻止 reservation 析构时撤销分配。
  block_reservation.KeepReserved();
  // 最后一次性发布候选计划，保证成功返回时 output 一定闭合。
  *output = std::move(candidate);
  return 0;
}

}  // namespace eufs::metadata
