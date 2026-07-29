#include "checker/consistency_report.h"

#include <sstream>

namespace eufs::checker {
namespace {

std::string_view ChineseVerdict(CheckVerdict verdict) {
  switch (verdict) {
    case CheckVerdict::kHealthy:
      return "当前检查规则内健康";
    case CheckVerdict::kInconsistent:
      return "发现一致性问题";
    case CheckVerdict::kIncomplete:
      return "扫描不完整，不能给出完整结论";
  }
  return "未知";
}

std::string_view ChineseCompleteness(bool complete) {
  return complete ? "完整" : "不完整";
}

std::string_view ChineseScanStatus(ScanStatus status) {
  switch (status) {
    case ScanStatus::kComplete:
      return "完整";
    case ScanStatus::kPartial:
      return "部分完成";
    case ScanStatus::kAborted:
      return "已中止";
  }
  return "未知";
}

void AppendJsonString(std::string_view value, std::ostringstream* output) {
  *output << '"';
  constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"':
        *output << "\\\"";
        break;
      case '\\':
        *output << "\\\\";
        break;
      case '\b':
        *output << "\\b";
        break;
      case '\f':
        *output << "\\f";
        break;
      case '\n':
        *output << "\\n";
        break;
      case '\r':
        *output << "\\r";
        break;
      case '\t':
        *output << "\\t";
        break;
      default:
        if (byte < 0x20U) {
          *output << "\\u00" << kHex[byte >> 4U] << kHex[byte & 0x0fU];
        } else {
          *output << static_cast<char>(byte);
        }
    }
  }
  *output << '"';
}

}  // namespace

bool IsEvidenceComplete(const ConsistencyReport& report) {
  return report.status == ScanStatus::kComplete &&
         report.bitmap_geometry_scan_complete && report.root_inode_decoded &&
         report.root_reachability_complete &&
         report.block_reference_scan_complete &&
         report.inode_reference_scan_complete;
}

CheckVerdict ClassifyReport(const ConsistencyReport& report) {
  if (!IsEvidenceComplete(report)) {
    return CheckVerdict::kIncomplete;
  }
  return report.issues.empty() ? CheckVerdict::kHealthy
                               : CheckVerdict::kInconsistent;
}

std::string_view ScanStatusName(ScanStatus status) {
  switch (status) {
    case ScanStatus::kComplete:
      return "complete";
    case ScanStatus::kPartial:
      return "partial";
    case ScanStatus::kAborted:
      return "aborted";
  }
  return "unknown";
}

std::string_view CheckVerdictName(CheckVerdict verdict) {
  switch (verdict) {
    case CheckVerdict::kHealthy:
      return "healthy";
    case CheckVerdict::kInconsistent:
      return "inconsistent";
    case CheckVerdict::kIncomplete:
      return "incomplete";
  }
  return "unknown";
}

std::string_view IssueCodeName(IssueCode code) {
  switch (code) {
    case IssueCode::kJournalRecoveryRequired:
      return "JOURNAL_RECOVERY_REQUIRED";
    case IssueCode::kMetadataBlockMarkedFree:
      return "METADATA_BLOCK_MARKED_FREE";
    case IssueCode::kInodeBitmapTailNotReserved:
      return "INODE_BITMAP_TAIL_NOT_RESERVED";
    case IssueCode::kBlockBitmapTailNotReserved:
      return "BLOCK_BITMAP_TAIL_NOT_RESERVED";
    case IssueCode::kRootInodeMarkedFree:
      return "ROOT_INODE_MARKED_FREE";
    case IssueCode::kRootInodeUndecodable:
      return "ROOT_INODE_UNDECODABLE";
    case IssueCode::kRootInodeNotDirectory:
      return "ROOT_INODE_NOT_DIRECTORY";
    case IssueCode::kDirectoryBlockOutOfRange:
      return "DIRECTORY_BLOCK_OUT_OF_RANGE";
    case IssueCode::kDirectoryBlockMarkedFree:
      return "DIRECTORY_BLOCK_MARKED_FREE";
    case IssueCode::kDirectoryRecordUndecodable:
      return "DIRECTORY_RECORD_UNDECODABLE";
    case IssueCode::kDentryTargetOutOfRange:
      return "DENTRY_TARGET_OUT_OF_RANGE";
    case IssueCode::kDentryTargetInodeMarkedFree:
      return "DENTRY_TARGET_INODE_MARKED_FREE";
    case IssueCode::kDentryTargetInodeUndecodable:
      return "DENTRY_TARGET_INODE_UNDECODABLE";
    case IssueCode::kAllocatedInodeUndecodable:
      return "ALLOCATED_INODE_UNDECODABLE";
    case IssueCode::kInodeUnreachable:
      return "INODE_UNREACHABLE";
    case IssueCode::kDirectoryCycle:
      return "DIRECTORY_CYCLE";
    case IssueCode::kDirectoryHardLink:
      return "DIRECTORY_HARD_LINK";
    case IssueCode::kDentryTypeMismatch:
      return "DENTRY_TYPE_MISMATCH";
    case IssueCode::kDuplicateDentryName:
      return "DUPLICATE_DENTRY_NAME";
    case IssueCode::kReferencedBlockOutOfRange:
      return "REFERENCED_BLOCK_OUT_OF_RANGE";
    case IssueCode::kReferencedBlockMarkedFree:
      return "REFERENCED_BLOCK_MARKED_FREE";
    case IssueCode::kMissingRequiredBlock:
      return "MISSING_REQUIRED_BLOCK";
    case IssueCode::kBlockPointerBeyondEnd:
      return "BLOCK_POINTER_BEYOND_END";
    case IssueCode::kDuplicateBlockReference:
      return "DUPLICATE_BLOCK_REFERENCE";
    case IssueCode::kAllocatedDataBlockUnreferenced:
      return "ALLOCATED_DATA_BLOCK_UNREFERENCED";
    case IssueCode::kInodeLinkCountMismatch:
      return "INODE_LINK_COUNT_MISMATCH";
    case IssueCode::kRegularFileHardLink:
      return "REGULAR_FILE_HARD_LINK";
    case IssueCode::kRootDirectoryReferenced:
      return "ROOT_DIRECTORY_REFERENCED";
  }
  return "UNKNOWN_ISSUE";
}

std::string_view IssueChineseMessage(IssueCode code) {
  switch (code) {
    case IssueCode::kJournalRecoveryRequired:
      return "日志中存在尚未清理的事务，必须先完成恢复";
    case IssueCode::kMetadataBlockMarkedFree:
      return "metadata 保留块在 block bitmap 中被标记为空闲";
    case IssueCode::kInodeBitmapTailNotReserved:
      return "inode bitmap 尾部包含可分配位";
    case IssueCode::kBlockBitmapTailNotReserved:
      return "block bitmap 尾部包含可分配位";
    case IssueCode::kRootInodeMarkedFree:
      return "根 inode 在 inode 位图中被标记为空闲";
    case IssueCode::kRootInodeUndecodable:
      return "根 inode 无法解码";
    case IssueCode::kRootInodeNotDirectory:
      return "根 inode 的实际类型不是目录";
    case IssueCode::kDirectoryBlockOutOfRange:
      return "目录数据块指针超出数据区";
    case IssueCode::kDirectoryBlockMarkedFree:
      return "目录引用的数据块在块位图中被标记为空闲";
    case IssueCode::kDirectoryRecordUndecodable:
      return "目录记录无法完整解析";
    case IssueCode::kDentryTargetOutOfRange:
      return "目录项指向 inode 表范围之外";
    case IssueCode::kDentryTargetInodeMarkedFree:
      return "目录项目标 inode 在位图中被标记为空闲";
    case IssueCode::kDentryTargetInodeUndecodable:
      return "目录项目标 inode 无法解码";
    case IssueCode::kAllocatedInodeUndecodable:
      return "位图声明已分配的 inode 无法解码";
    case IssueCode::kInodeUnreachable:
      return "已分配 inode 无法从根目录到达";
    case IssueCode::kDirectoryCycle:
      return "目录图中存在环";
    case IssueCode::kDirectoryHardLink:
      return "目录 inode 存在多个命名空间入口";
    case IssueCode::kDentryTypeMismatch:
      return "目录项类型与目标 inode 类型不一致";
    case IssueCode::kDuplicateDentryName:
      return "同一目录中存在重复的有效名称";
    case IssueCode::kReferencedBlockOutOfRange:
      return "inode 引用的数据块超出数据区";
    case IssueCode::kReferencedBlockMarkedFree:
      return "inode 引用的数据块在位图中被标记为空闲";
    case IssueCode::kMissingRequiredBlock:
      return "文件大小要求的数据块指针缺失";
    case IssueCode::kBlockPointerBeyondEnd:
      return "inode 在文件结尾之后仍保留块指针";
    case IssueCode::kDuplicateBlockReference:
      return "同一物理块被多个逻辑位置引用";
    case IssueCode::kAllocatedDataBlockUnreferenced:
      return "数据块已分配但没有任何 inode 引用";
    case IssueCode::kInodeLinkCountMismatch:
      return "inode 链接计数与命名空间证据不一致";
    case IssueCode::kRegularFileHardLink:
      return "普通文件存在 v1 不支持的硬链接";
    case IssueCode::kRootDirectoryReferenced:
      return "磁盘目录项非法指向根 inode";
  }
  return "未知一致性问题";
}

std::string FormatHumanReport(std::string_view image_path,
                              const ConsistencyReport& report) {
  const CheckVerdict verdict = ClassifyReport(report);
  std::ostringstream output;
  output << "eufsck 镜像: " << image_path << '\n'
         << "结论: " << ChineseVerdict(verdict) << '\n'
         << "扫描状态: " << ChineseScanStatus(report.status) << '\n'
         << "证据门禁:\n"
         << "  bitmap 几何: "
         << ChineseCompleteness(report.bitmap_geometry_scan_complete) << '\n'
         << "  根 inode 解码: "
         << ChineseCompleteness(report.root_inode_decoded) << '\n'
         << "  根目录可达性: "
         << ChineseCompleteness(report.root_reachability_complete) << '\n'
         << "  块引用扫描: "
         << ChineseCompleteness(report.block_reference_scan_complete) << '\n'
         << "  inode 引用计数扫描: "
         << ChineseCompleteness(report.inode_reference_scan_complete) << '\n'
         << "统计: 物理扫描 inode="
         << report.physical_scan_inode_numbers.size()
         << ", 根可达 inode=" << report.root_reachable_inode_numbers.size()
         << ", 块引用=" << report.block_references.size()
         << ", 问题=" << report.issues.size() << '\n';
  for (const CheckIssue& issue : report.issues) {
    output << "- [" << IssueCodeName(issue.code) << "] "
           << IssueChineseMessage(issue.code) << " (inode="
           << issue.inode_number << ", block=" << issue.block_number
           << ", related_inode=" << issue.related_inode_number << ")\n";
    if (issue.occurrence_count > 1U || issue.first_bitmap_bit != 0U) {
      output << "  bitmap: first_bit=" << issue.first_bitmap_bit
             << ", occurrences=" << issue.occurrence_count << '\n';
    }
    if (!issue.detail.empty()) {
      output << "  证据: " << issue.detail << '\n';
    }
  }
  return output.str();
}

std::string FormatJsonReport(std::string_view image_path,
                             const ConsistencyReport& report) {
  const CheckVerdict verdict = ClassifyReport(report);
  std::ostringstream output;
  output << "{\"schema_version\":1,\"image\":";
  AppendJsonString(image_path, &output);
  output << ",\"verdict\":";
  AppendJsonString(CheckVerdictName(verdict), &output);
  output << ",\"scan_status\":";
  AppendJsonString(ScanStatusName(report.status), &output);
  output << ",\"evidence\":{\"bitmap_geometry_scan_complete\":"
         << (report.bitmap_geometry_scan_complete ? "true" : "false")
         << ",\"root_inode_decoded\":"
         << (report.root_inode_decoded ? "true" : "false")
         << ",\"root_reachability_complete\":"
         << (report.root_reachability_complete ? "true" : "false")
         << ",\"block_reference_scan_complete\":"
         << (report.block_reference_scan_complete ? "true" : "false")
         << ",\"inode_reference_scan_complete\":"
         << (report.inode_reference_scan_complete ? "true" : "false")
         << "},\"counts\":{\"physical_scan_inodes\":"
         << report.physical_scan_inode_numbers.size()
         << ",\"root_reachable_inodes\":"
         << report.root_reachable_inode_numbers.size()
         << ",\"block_references\":" << report.block_references.size()
         << ",\"issues\":" << report.issues.size() << "},\"issues\":[";
  for (std::size_t index = 0; index < report.issues.size(); ++index) {
    if (index != 0) {
      output << ',';
    }
    const CheckIssue& issue = report.issues[index];
    output << "{\"code\":";
    AppendJsonString(IssueCodeName(issue.code), &output);
    output << ",\"message\":";
    AppendJsonString(IssueChineseMessage(issue.code), &output);
    output << ",\"inode\":" << issue.inode_number
           << ",\"block\":" << issue.block_number
           << ",\"related_inode\":" << issue.related_inode_number
           << ",\"first_bitmap_bit\":" << issue.first_bitmap_bit
           << ",\"occurrences\":" << issue.occurrence_count
           << ",\"detail\":";
    AppendJsonString(issue.detail, &output);
    output << '}';
  }
  output << "]}\n";
  return output.str();
}

std::string FormatJsonError(std::string_view image_path, int error_number,
                            std::string_view detail) {
  std::ostringstream output;
  output << "{\"schema_version\":1,\"image\":";
  AppendJsonString(image_path, &output);
  output << ",\"verdict\":\"error\",\"error\":{\"errno\":"
         << error_number << ",\"detail\":";
  AppendJsonString(detail, &output);
  output << "}}\n";
  return output.str();
}

}  // namespace eufs::checker
