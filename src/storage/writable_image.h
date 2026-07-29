#pragma once

#include "metadata/empty_file_create_plan.h"
#include "metadata/file_write_plan.h"
#include "metadata/first_block_write_plan.h"

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
