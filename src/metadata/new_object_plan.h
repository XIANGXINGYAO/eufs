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

bool IsValidRootObjectName(std::string_view name);

struct NewObjectPlan {
  std::uint32_t inode_number{0};
  std::uint64_t object_size{0};
  std::uint32_t directory_block{0};
  bool directory_grew{false};
  std::vector<std::uint32_t> data_blocks;
  std::uint32_t file_indirect_block{0};
  std::uint32_t root_indirect_block{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> ordered_data_after_images;
  std::map<std::uint32_t, ondisk::Block> metadata_after_images;
};

int PrepareNewRootObject(const storage::ImageReader& image,
                         std::string_view name, std::string_view data,
                         std::uint32_t permissions, std::uint32_t uid,
                         std::uint32_t gid, std::uint64_t timestamp_ns,
                         NewObjectPlan* output, std::string* detail);

}  // namespace eufs::metadata
