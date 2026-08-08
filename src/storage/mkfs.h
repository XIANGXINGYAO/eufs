#pragma once

#include "metadata/ondisk_format.h"

#include <cstdint>
#include <string>

namespace eufs::storage {

// eufs-mkfs 命令层解析后的格式化参数及默认值。
struct MkfsOptions {
  // 目标镜像路径和预分配字节数。
  std::string image_path;
  std::uint64_t image_size_bytes{0};
  std::uint32_t total_inodes{1024};
  std::uint32_t journal_blocks{256};
  // 0 暂时表示不创建；非零值必须是每块记录数 32 的整数倍。
  // Request Ledger 完成启动扫描和访问隔离后，CLI 才会公开该选项。
  std::uint32_t request_ledger_entries{0};
  // force=false 时拒绝覆盖已经存在的路径。
  bool force{false};
};

// 创建、预分配并初始化完整 EUFS v1 镜像；superblock 最后发布，
// 因此中途失败的部分文件不会被 reader 误认成已格式化镜像。
bool FormatImage(const MkfsOptions& options,
                 ondisk::Superblock* formatted_superblock,
                 std::string* error);

}  // namespace eufs::storage
