#pragma once

#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace eufs::metadata {

struct FileWritePlan {
  std::uint32_t inode_number{0};
  std::uint64_t old_size{0};
  std::uint64_t new_size{0};
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> ordered_data_after_images;
  std::map<std::uint32_t, ondisk::Block> metadata_after_images;
};

int PrepareFileWrite(const storage::ImageReader& image,
                     std::uint32_t inode_number, std::uint64_t offset,
                     std::string_view data, std::uint64_t timestamp_ns,
                     FileWritePlan* output, std::string* detail);

}  // namespace eufs::metadata
