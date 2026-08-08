#include "checker/eufsck_command.h"

#include "checker/consistency_checker.h"
#include "checker/consistency_report.h"

#include <ostream>
#include <string_view>

namespace eufs::checker {
namespace {

// 打印稳定中文命令格式。
void PrintUsage(std::ostream& output) {
  output << "用法: eufsck [--json] IMAGE\n"
            "离线、只读检查 eufs 镜像；检查前必须停止 eufsd。\n";
}

}  // 匿名命名空间。

// 解析 eufsck 参数，调用只读扫描器，格式化报告并映射稳定退出码。
int RunEufsck(const std::vector<std::string>& arguments, std::ostream& output,
              std::ostream& error_output) {
  bool json = false;
  // `--` 之后的字符串即使以 `-` 开头也按镜像路径处理。
  bool positional_only = false;
  std::string image_path;

  // 只允许一个 IMAGE 位置参数和最多一个 --json。
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

  // CheckImage 只返回运行错误；发现结构问题通过 report.issues 表达。
  ConsistencyReport report;
  std::string detail;
  const int result = CheckImage(image_path, &report, &detail);
  // 运行错误与“不一致”不同：前者没有足够证据生成正常 verdict。
  if (result != 0) {
    if (json) {
      output << FormatJsonError(image_path, -result, detail);
    } else {
      error_output << "eufsck: 无法检查镜像 " << image_path << ": "
                   << detail << " (errno " << -result << ")\n";
    }
    return kEufsckExitRuntimeError;
  }

  // 同一份结构化报告可序列化为人类文本或 JSON。
  output << (json ? FormatJsonReport(image_path, report)
                  : FormatHumanReport(image_path, report));
  // 最终 verdict 映射成脚本可依赖的稳定退出码。
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
