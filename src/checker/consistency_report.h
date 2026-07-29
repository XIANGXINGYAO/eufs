#pragma once

#include "checker/consistency_checker.h"

#include <string>
#include <string_view>

namespace eufs::checker {

enum class CheckVerdict {
  kHealthy,
  kInconsistent,
  kIncomplete,
};

bool IsEvidenceComplete(const ConsistencyReport& report);
CheckVerdict ClassifyReport(const ConsistencyReport& report);

std::string_view ScanStatusName(ScanStatus status);
std::string_view CheckVerdictName(CheckVerdict verdict);
std::string_view IssueCodeName(IssueCode code);
std::string_view IssueChineseMessage(IssueCode code);

std::string FormatHumanReport(std::string_view image_path,
                              const ConsistencyReport& report);
std::string FormatJsonReport(std::string_view image_path,
                             const ConsistencyReport& report);
std::string FormatJsonError(std::string_view image_path, int error_number,
                            std::string_view detail);

}  // namespace eufs::checker
