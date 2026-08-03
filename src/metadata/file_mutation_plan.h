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

// 所有已有普通文件内容变更共用的磁盘事务计划。
struct FileMutationPlan {
  std::uint32_t inode_number{0};
  std::uint64_t old_generation{0};
  std::uint64_t new_generation{0};
  std::uint64_t old_size{0};
  std::uint64_t new_size{0};
  std::vector<std::uint32_t> old_data_blocks;
  std::vector<std::uint32_t> new_data_blocks;
  std::uint32_t old_indirect_block{0};
  std::uint32_t new_indirect_block{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> ordered_data_after_images;
  std::map<std::uint32_t, ondisk::Block> metadata_after_images;
};

namespace detail {

enum class FileMutationMode {
  kWriteRange,
  kReplaceAll,
};

// 只在 planner 调用期间存在的语义请求；磁盘转换规则不由上层重复实现。
struct FileMutationRequest {
  FileMutationMode mode{FileMutationMode::kWriteRange};
  std::uint64_t offset{0};
  std::string_view data;
  std::uint64_t timestamp_ns{0};
  std::uint64_t expected_generation{0};
};

// 统一执行旧映射读取、目标映射构造、COW、bitmap/indirect/inode 发布计划。
int PrepareFileMutation(const storage::ImageReader& image,
                        std::uint32_t inode_number,
                        const FileMutationRequest& request,
                        FileMutationPlan* output,
                        std::string* detail);

}  // namespace detail

}  // namespace eufs::metadata
