#include "checker/eufsck_command.h"

#include "checker/consistency_checker.h"
#include "checker/consistency_report.h"

#include <ostream>
#include <string_view>

namespace eufs::checker {
namespace {

void PrintUsage(std::ostream& output) {
  output << "用法: eufsck [--json] IMAGE\n"
            "离线、只读检查 eufs 镜像；检查前必须停止 eufsd。\n";
}

}  // namespace

int RunEufsck(const std::vector<std::string>& arguments, std::ostream& output,
              std::ostream& error_output) {
  bool json = false;
  bool positional_only = false;
  std::string image_path;

  for (const std::string& argument : arguments) {
    if (!positional_only && (argument == "--help" || argument == "-h")) {
      if (arguments.size() != 1U) {
        PrintUsage(error_output);
        return kEufsckExitUsage;
      }
      PrintUsage(output);
      return kEufsckExitHealthy;
    }
    if (!positional_only && argument == "--json") {
      if (json) {
        PrintUsage(error_output);
        return kEufsckExitUsage;
      }
      json = true;
      continue;
    }
    if (!positional_only && argument == "--") {
      positional_only = true;
      continue;
    }
    if (!positional_only && !argument.empty() && argument.front() == '-') {
      PrintUsage(error_output);
      return kEufsckExitUsage;
    }
    if (!image_path.empty() || argument.empty()) {
      PrintUsage(error_output);
      return kEufsckExitUsage;
    }
    image_path = argument;
  }

  if (image_path.empty()) {
    PrintUsage(error_output);
    return kEufsckExitUsage;
  }

  ConsistencyReport report;
  std::string detail;
  const int result = CheckImage(image_path, &report, &detail);
  if (result != 0) {
    if (json) {
      output << FormatJsonError(image_path, -result, detail);
    } else {
      error_output << "eufsck: 无法检查镜像 " << image_path << ": "
                   << detail << " (errno " << -result << ")\n";
    }
    return kEufsckExitRuntimeError;
  }

  output << (json ? FormatJsonReport(image_path, report)
                  : FormatHumanReport(image_path, report));
  switch (ClassifyReport(report)) {
    case CheckVerdict::kHealthy:
      return kEufsckExitHealthy;
    case CheckVerdict::kInconsistent:
      return kEufsckExitInconsistent;
    case CheckVerdict::kIncomplete:
      return kEufsckExitIncomplete;
  }
  return kEufsckExitRuntimeError;
}

}  // namespace eufs::checker
