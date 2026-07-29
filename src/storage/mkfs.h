#pragma once

#include "metadata/ondisk_format.h"

#include <cstdint>
#include <string>

namespace eufs::storage {

struct MkfsOptions {
  std::string image_path;
  std::uint64_t image_size_bytes{0};
  std::uint32_t total_inodes{1024};
  std::uint32_t journal_blocks{256};
  bool force{false};
};

bool FormatImage(const MkfsOptions& options,
                 ondisk::Superblock* formatted_superblock,
                 std::string* error);

}  // namespace eufs::storage
