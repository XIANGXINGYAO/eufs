#pragma once

#include "metadata/file_mutation_plan.h"

#include <string>
#include <string_view>

namespace eufs::metadata {

// POSIX write 与对象替换共享同一种磁盘计划，只保留不同的入口语义。
using FileWritePlan = FileMutationPlan;

// 保留未覆盖旧字节、处理 EOF gap 和增长；它是 POSIX 局部写，不是整对象替换。
int PrepareFileWrite(const storage::ImageReader& image,
                     std::uint32_t inode_number, std::uint64_t offset,
                     std::string_view data, std::uint64_t timestamp_ns,
                     FileWritePlan* output, std::string* detail);

}  // namespace eufs::metadata
