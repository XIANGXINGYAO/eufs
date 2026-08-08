#pragma once

// 早期“空文件第一次写入”测试计划接口，仅供恢复与兼容性测试复现旧场景。
// 当前生产写路径使用 metadata/file_write_plan.h。
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace eufs::metadata {

struct FirstBlockWritePlan {
  std::uint32_t inode_number{0};
  std::uint32_t data_block{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> ordered_data_after_images;
  std::map<std::uint32_t, ondisk::Block> metadata_after_images;
};

int PrepareFirstBlockWrite(const storage::ImageReader& image,
                           std::uint32_t inode_number,
                           std::string_view data,
                           std::uint64_t timestamp_ns,
                           FirstBlockWritePlan* output,
                           std::string* detail);

}  // namespace eufs::metadata
