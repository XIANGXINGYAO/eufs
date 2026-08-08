#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace eufs::checker {

// eufsck 稳定进程退出码：0 健康，1 确认不一致，2 证据不完整，3 运行错误，64 用法错误。
constexpr int kEufsckExitHealthy = 0;
constexpr int kEufsckExitInconsistent = 1;
constexpr int kEufsckExitIncomplete = 2;
constexpr int kEufsckExitRuntimeError = 3;
constexpr int kEufsckExitUsage = 64;

// 可测试的命令入口：显式接收参数和输出流，不直接依赖全局 argc/stdout。
int RunEufsck(const std::vector<std::string>& arguments, std::ostream& output,
              std::ostream& error_output);

}  // namespace eufs::checker
