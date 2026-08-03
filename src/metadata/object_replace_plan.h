#pragma once

#include "metadata/file_mutation_plan.h"

#include <string>
#include <string_view>

namespace eufs::metadata {

// 对象接口沿用统一文件变更计划，不再维护第二套块分配算法。
using ObjectReplacePlan = FileMutationPlan;

// 仅当 inode 当前 generation 等于 expected_generation 时生成全量 COW 替换计划。
// ESTALE/EOVERFLOW/ENOSPC 等失败不会修改 output，也不会直接修改镜像。
int PrepareObjectReplace(const storage::ImageReader& image,
                         std::uint32_t inode_number,
                         std::uint64_t expected_generation,
                         std::string_view data,
                         std::uint64_t timestamp_ns,
                         ObjectReplacePlan* output,
                         std::string* detail);

}  // namespace eufs::metadata
