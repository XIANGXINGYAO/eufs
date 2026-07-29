#pragma once

#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"

#include <cstdint>
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace eufs::metadata {

struct EmptyFileCreatePlan {
  std::uint32_t inode_number{0};
  std::uint32_t directory_block{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> after_images;
  std::vector<std::uint32_t> write_order;
};

int PrepareRootEmptyFileCreate(const storage::ImageReader& image,
                               std::string_view name,
                               std::uint32_t permissions, std::uint32_t uid,
                               std::uint32_t gid, std::uint64_t timestamp_ns,
                               EmptyFileCreatePlan* output,
                               std::string* detail);

}  // namespace eufs::metadata
