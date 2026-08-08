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

// 在根目录创建一个空文件所需的完整 metadata 事务计划。
struct EmptyFileCreatePlan {
  // 新分配 inode 及实际写入目录项的目录数据块。
  std::uint32_t inode_number{0};
  std::uint32_t directory_block{0};
  // 用于执行前镜像身份/几何校验，防止把旧计划应用到另一镜像。
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  // home block -> 旧块/新块；两者共同进入日志执行器的比较与 redo 路径。
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> after_images;
  // 保留受影响块的确定性顺序，供直接应用测试验证原子计划内容。
  std::vector<std::uint32_t> write_order;
};

// 只读取 image 并在内存生成计划；失败时 output 保持原值，磁盘绝不改变。
int PrepareRootEmptyFileCreate(const storage::ImageReader& image,
                               std::string_view name,
                               std::uint32_t permissions, std::uint32_t uid,
                               std::uint32_t gid, std::uint64_t timestamp_ns,
                               EmptyFileCreatePlan* output,
                               std::string* detail);

}  // namespace eufs::metadata
