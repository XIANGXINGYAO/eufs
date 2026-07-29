#pragma once

#include <iosfwd>
#include <string>
#include <vector>

namespace eufs::checker {

constexpr int kEufsckExitHealthy = 0;
constexpr int kEufsckExitInconsistent = 1;
constexpr int kEufsckExitIncomplete = 2;
constexpr int kEufsckExitRuntimeError = 3;
constexpr int kEufsckExitUsage = 64;

int RunEufsck(const std::vector<std::string>& arguments, std::ostream& output,
              std::ostream& error_output);

}  // namespace eufs::checker
