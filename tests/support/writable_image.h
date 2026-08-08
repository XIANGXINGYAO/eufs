#pragma once

// 测试专用直接落盘接口；调用者必须明确接受它不经过 journal 的事实。
// 这些函数只用于生成 fixture，不属于挂载后可用能力。
#include "metadata/empty_file_create_plan.h"
#include "metadata/file_write_plan.h"
#include "tests/support/first_block_write_plan.h"

#include <string>

namespace eufs::storage {

int ApplyCreatePlan(const std::string& image_path,
                    const metadata::EmptyFileCreatePlan& plan,
                    std::string* detail);

// Consumes locked_fd on every return path without changing its flock state.
int ApplyCreatePlanOnLockedFd(int locked_fd,
                              const metadata::EmptyFileCreatePlan& plan,
                              std::string* detail);

int ApplyFirstBlockWritePlan(const std::string& image_path,
                             const metadata::FirstBlockWritePlan& plan,
                             std::string* detail);

// Consumes locked_fd on every return path without changing its flock state.
int ApplyFirstBlockWritePlanOnLockedFd(
    int locked_fd, const metadata::FirstBlockWritePlan& plan,
    std::string* detail);

int ApplyFileWritePlan(const std::string& image_path,
                       const metadata::FileWritePlan& plan,
                       std::string* detail);

// Consumes locked_fd on every return path without changing its flock state.
int ApplyFileWritePlanOnLockedFd(int locked_fd,
                                 const metadata::FileWritePlan& plan,
                                 std::string* detail);

}  // namespace eufs::storage
