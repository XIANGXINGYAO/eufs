#pragma once

#include "checker/consistency_checker.h"

#include <string>
#include <string_view>

namespace eufs::checker {

// 面向用户和进程退出码的最终结论，必须同时考虑问题列表和证据完整性。
enum class CheckVerdict {
  kHealthy,
  kInconsistent,
  kIncomplete,
};

// 判断支撑最终结论的全部独立扫描阶段是否完成。
bool IsEvidenceComplete(const ConsistencyReport& report);
// 完整且无问题为 healthy；完整且有问题为 inconsistent；证据不足为 incomplete。
CheckVerdict ClassifyReport(const ConsistencyReport& report);

// 稳定英文名称用于 JSON/自动化；中文消息用于人类可读报告。
std::string_view ScanStatusName(ScanStatus status);
std::string_view CheckVerdictName(CheckVerdict verdict);
std::string_view IssueCodeName(IssueCode code);
std::string_view IssueChineseMessage(IssueCode code);

// 两种成功扫描输出共享同一份 ConsistencyReport，只改变序列化格式。
std::string FormatHumanReport(std::string_view image_path,
                              const ConsistencyReport& report);
std::string FormatJsonReport(std::string_view image_path,
                             const ConsistencyReport& report);
// 运行时错误无法产生 report，单独编码镜像、errno 和 detail。
std::string FormatJsonError(std::string_view image_path, int error_number,
                            std::string_view detail);

}  // namespace eufs::checker
