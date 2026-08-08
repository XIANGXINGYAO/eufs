#pragma once

#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace eufs::metadata {

// 对象名直接映射根目录单个文件名，因此不允许斜杠、`.`、`..` 或超长名称。
bool IsValidRootObjectName(std::string_view name);

// 原子发布一个此前不存在的完整对象所需计划。
struct NewObjectPlan {
  std::uint32_t inode_number{0};
  std::uint64_t object_size{0};
  std::uint32_t directory_block{0};
  // directory_grew 表示原目录块无空间，计划新增了根目录数据块。
  bool directory_grew{false};
  // 新对象全部数据块，顺序对应逻辑块 0..N-1。
  std::vector<std::uint32_t> data_blocks;
  // 文件或根目录跨过 12 个直接块时各自新分配的间接块。
  std::uint32_t file_indirect_block{0};
  std::uint32_t root_indirect_block{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  // 数据先落盘；inode、目录项、bitmap 和间接索引通过同一 COMMIT 发布。
  std::map<std::uint32_t, ondisk::Block> ordered_data_after_images;
  std::map<std::uint32_t, ondisk::Block> metadata_after_images;
};

// name 已存在返回 EEXIST；ENOSPC 或其他失败不会产生部分计划或磁盘修改。
int PrepareNewRootObject(const storage::ImageReader& image,
                         std::string_view name, std::string_view data,
                         std::uint32_t permissions, std::uint32_t uid,
                         std::uint32_t gid, std::uint64_t timestamp_ns,
                         NewObjectPlan* output, std::string* detail);

}  // namespace eufs::metadata
