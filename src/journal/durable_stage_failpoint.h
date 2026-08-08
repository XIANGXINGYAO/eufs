#pragma once

#include "journal/journal_control_store.h"

#include <memory>
#include <string>
#include <string_view>

namespace eufs::journal {

// 持久化阶段与命令行名称使用唯一映射，FUSE 和 brpc 故障矩阵共同复用。
const char* DurableStageName(DurableStage stage);

// 把命令行阶段名称解析为执行器真正上报的持久化边界。
bool ParseDurableStage(std::string_view value, DurableStage* output);

// 创建测试专用进程崩溃观察者；命中目标阶段时立即 _exit(200)。
std::shared_ptr<DurableStageObserver> MakeProcessCrashObserver(
    DurableStage target, std::string process_name);

}  // namespace eufs::journal
