#pragma once

#include "metadata/ondisk_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace eufs::checker {

// 描述扫描证据是否覆盖了所有计划检查的区域。
enum class ScanStatus {
  // 所有独立扫描阶段均完成，可以据此给出健康/不一致结论。
  kComplete,
  // 遇到局部损坏后跳过该局部，但仍继续收集其他区域证据。
  kPartial,
  // 全局前提失败，例如镜像无法打开或 superblock 不可用，无法继续扫描。
  kAborted,
};

// 每种可机器识别的一致性问题；报告层再映射为中文文本和 JSON 名称。
enum class IssueCode {
  kJournalRecoveryRequired,
  kMetadataBlockMarkedFree,
  kInodeBitmapTailNotReserved,
  kBlockBitmapTailNotReserved,
  kRootInodeMarkedFree,
  kRootInodeUndecodable,
  kRootInodeNotDirectory,
  kDirectoryBlockOutOfRange,
  kDirectoryBlockMarkedFree,
  kDirectoryRecordUndecodable,
  kDentryTargetOutOfRange,
  kDentryTargetInodeMarkedFree,
  kDentryTargetInodeUndecodable,
  kAllocatedInodeUndecodable,
  kInodeUnreachable,
  kDirectoryCycle,
  kDirectoryHardLink,
  kDentryTypeMismatch,
  kDuplicateDentryName,
  kReferencedBlockOutOfRange,
  kReferencedBlockMarkedFree,
  kMissingRequiredBlock,
  kBlockPointerBeyondEnd,
  kDuplicateBlockReference,
  kAllocatedDataBlockUnreferenced,
  kInodeLinkCountMismatch,
  kRegularFileHardLink,
  kRootDirectoryReferenced,
  kRequestLedgerIdentityInvalid,
  kRequestLedgerGeometryInvalid,
  kRequestLedgerBlockUnavailable,
  kRequestLedgerSlotCorrupt,
  kRequestLedgerRecordAfterHole,
  kRequestLedgerDuplicateRequestId,
};

// 区分 inode 对块的引用是文件数据还是 single-indirect 索引块。
enum class BlockReferenceKind {
  kData,
  kIndirect,
};

// 一条结构化问题证据；未使用的字段保持 0。
struct CheckIssue {
  IssueCode code{IssueCode::kRootInodeMarkedFree};
  std::uint32_t inode_number{0};
  std::uint32_t block_number{0};
  std::uint32_t related_inode_number{0};
  std::string detail;
  // 相同位图尾部问题可聚合为起始 bit + occurrence_count，避免产生海量记录。
  std::uint64_t first_bitmap_bit{0};
  std::uint64_t occurrence_count{1};
  // Request Ledger 问题使用从 1 开始的全局槽位；非 ledger 问题保持 0。
  std::uint64_t ledger_slot{0};
};

// 物理遍历 inode table 后得到的 inode 证据，与命名空间可达性分开计算。
struct DiscoveredInode {
  std::uint32_t inode_number{0};
  bool bitmap_allocated{false};
  ondisk::InodeRecord inode;
};

// 从一个目录项推导出的父目录 -> 子 inode 图边。
struct DirectoryEdge {
  std::uint32_t parent_inode_number{0};
  std::uint32_t child_inode_number{0};
};

// 记录某目录是否被完整解码；局部损坏时 inode 可发现但 complete 为 false。
struct DirectoryScanState {
  std::uint32_t inode_number{0};
  bool complete{false};
};

// 一个物理块被哪个 inode 的哪个逻辑位置引用。
struct BlockReference {
  std::uint32_t block_number{0};
  std::uint32_t inode_number{0};
  std::uint32_t logical_block{0};
  BlockReferenceKind kind{BlockReferenceKind::kData};
};

// link_count 交叉验证需要的声明值、实际目录项引用和子目录数量。
struct InodeLinkObservation {
  std::uint32_t inode_number{0};
  std::uint32_t declared_link_count{0};
  std::uint64_t observed_dentry_references{0};
  std::uint64_t observed_child_directories{0};
  std::uint64_t expected_link_count{0};
  bool expectation_complete{false};
};

// eufsck 的完整结构化证据，格式化层只消费它，不重新扫描镜像。
struct ConsistencyReport {
  // 各 complete 标志分别约束不同结论，不能用一个局部成功代替全局完成。
  ScanStatus status{ScanStatus::kAborted};
  bool bitmap_geometry_scan_complete{false};
  bool root_inode_decoded{false};
  bool root_reachability_complete{false};
  bool block_reference_scan_complete{false};
  bool inode_reference_scan_complete{false};
  // 旧镜像不声明 ledger 时该阶段不适用，并视为完整；声明后必须完成身份和槽扫描。
  bool request_ledger_feature_enabled{false};
  bool request_ledger_scan_complete{false};
  std::uint64_t request_ledger_capacity{0};
  std::uint64_t request_ledger_slots_scanned{0};
  std::uint64_t request_ledger_valid_records{0};
  std::uint64_t request_ledger_empty_slots{0};
  ondisk::InodeRecord root_inode;
  std::vector<ondisk::DirectoryEntry> root_entries;
  std::vector<DiscoveredInode> discovered_inodes;
  std::vector<DirectoryEdge> directory_edges;
  std::vector<DirectoryScanState> directory_scan_states;
  std::vector<BlockReference> block_references;
  std::vector<InodeLinkObservation> inode_link_observations;
  std::vector<std::uint32_t> allocated_unreferenced_data_blocks;
  // 物理扫描集合和根可达集合独立保存，差集才是不可达 inode。
  std::vector<std::uint32_t> physical_scan_inode_numbers;
  std::vector<std::uint32_t> root_reachable_inode_numbers;
  std::vector<CheckIssue> issues;
};

// 要求镜像 journal 已处于稳定干净状态；随后先扫物理 inode 证据，再构建目录图，
// 最后独立推导根可达集合。局部矛盾加入 issues，不因单个错误提前停止全局扫描。
int CheckImage(const std::string& image_path, ConsistencyReport* output,
               std::string* detail);

}  // namespace eufs::checker
