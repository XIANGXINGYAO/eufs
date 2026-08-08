#include "checker/consistency_report.h"

#include <sstream>

namespace eufs::checker {
namespace {

// 中文显示名称与稳定英文枚举名称分开，避免修改文案破坏自动化接口。
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

// 正确转义 JSON 字符串中的引号、反斜杠和控制字符。
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

}  // 匿名命名空间：报告格式化辅助函数不导出。

// 只有所有独立证据阶段均完成，才能把“未发现问题”解释为健康。
bool IsEvidenceComplete(const ConsistencyReport& report) {
  return report.status == ScanStatus::kComplete &&
         report.bitmap_geometry_scan_complete && report.root_inode_decoded &&
         report.root_reachability_complete &&
         report.block_reference_scan_complete &&
         report.inode_reference_scan_complete &&
         report.request_ledger_scan_complete;
}

// 证据不完整优先级最高；完整后再根据 issues 是否为空区分健康/不一致。
CheckVerdict ClassifyReport(const ConsistencyReport& report) {
  if (!IsEvidenceComplete(report)) {
    return CheckVerdict::kIncomplete;
  }
  return report.issues.empty() ? CheckVerdict::kHealthy
                               : CheckVerdict::kInconsistent;
}

// 以下 Name 函数返回机器稳定英文标识，ChineseMessage 返回用户文案。
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
    case IssueCode::kRequestLedgerIdentityInvalid:
      return "REQUEST_LEDGER_IDENTITY_INVALID";
    case IssueCode::kRequestLedgerGeometryInvalid:
      return "REQUEST_LEDGER_GEOMETRY_INVALID";
    case IssueCode::kRequestLedgerBlockUnavailable:
      return "REQUEST_LEDGER_BLOCK_UNAVAILABLE";
    case IssueCode::kRequestLedgerSlotCorrupt:
      return "REQUEST_LEDGER_SLOT_CORRUPT";
    case IssueCode::kRequestLedgerRecordAfterHole:
      return "REQUEST_LEDGER_RECORD_AFTER_HOLE";
    case IssueCode::kRequestLedgerDuplicateRequestId:
      return "REQUEST_LEDGER_DUPLICATE_REQUEST_ID";
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
    case IssueCode::kRequestLedgerIdentityInvalid:
      return "Request Ledger 的固定名称或 inode 身份不一致";
    case IssueCode::kRequestLedgerGeometryInvalid:
      return "Request Ledger 的文件类型、链接数或容量不合法";
    case IssueCode::kRequestLedgerBlockUnavailable:
      return "Request Ledger 的部分逻辑块无法读取";
    case IssueCode::kRequestLedgerSlotCorrupt:
      return "Request Ledger 槽位不是全零空槽或合法记录";
    case IssueCode::kRequestLedgerRecordAfterHole:
      return "Request Ledger 在空槽之后又出现合法记录";
    case IssueCode::kRequestLedgerDuplicateRequestId:
      return "Request Ledger 中出现重复 Request-ID";
  }
  return "未知一致性问题";
}

// 生成可直接阅读的中文多行报告，同时显式展示各阶段完整性。
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
         << "  Request Ledger 扫描: "
         << ChineseCompleteness(report.request_ledger_scan_complete)
         << (report.request_ledger_feature_enabled ? "（已启用）" : "（不适用）")
         << '\n'
         << "统计: 物理扫描 inode="
         << report.physical_scan_inode_numbers.size()
         << ", 根可达 inode=" << report.root_reachable_inode_numbers.size()
         << ", 块引用=" << report.block_references.size()
         << ", ledger 槽位=" << report.request_ledger_slots_scanned << '/'
         << report.request_ledger_capacity
         << ", ledger 记录=" << report.request_ledger_valid_records
         << ", 问题=" << report.issues.size() << '\n';
  for (const CheckIssue& issue : report.issues) {
    output << "- [" << IssueCodeName(issue.code) << "] "
           << IssueChineseMessage(issue.code) << " (inode="
           << issue.inode_number << ", block=" << issue.block_number
           << ", related_inode=" << issue.related_inode_number;
    if (issue.ledger_slot != 0) {
      output << ", ledger_slot=" << issue.ledger_slot;
    }
    output << ")\n";
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

// 生成结构稳定的 JSON 报告，不依赖第三方 JSON 库。
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
         << ",\"request_ledger_feature_enabled\":"
         << (report.request_ledger_feature_enabled ? "true" : "false")
         << ",\"request_ledger_scan_complete\":"
         << (report.request_ledger_scan_complete ? "true" : "false")
         << "},\"counts\":{\"physical_scan_inodes\":"
         << report.physical_scan_inode_numbers.size()
         << ",\"root_reachable_inodes\":"
         << report.root_reachable_inode_numbers.size()
         << ",\"block_references\":" << report.block_references.size()
         << ",\"request_ledger_capacity\":"
         << report.request_ledger_capacity
         << ",\"request_ledger_slots_scanned\":"
         << report.request_ledger_slots_scanned
         << ",\"request_ledger_valid_records\":"
         << report.request_ledger_valid_records
         << ",\"request_ledger_empty_slots\":"
         << report.request_ledger_empty_slots
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
           << ",\"ledger_slot\":" << issue.ledger_slot
           << ",\"detail\":";
    AppendJsonString(issue.detail, &output);
    output << '}';
  }
  output << "]}\n";
  return output.str();
}

// 镜像无法扫描时生成独立 JSON error 对象。
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
