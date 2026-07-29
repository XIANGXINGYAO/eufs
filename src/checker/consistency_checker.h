#pragma once

#include "metadata/ondisk_format.h"

#include <cstdint>
#include <string>
#include <vector>

namespace eufs::checker {

enum class ScanStatus {
  kComplete,
  kPartial,
  kAborted,
};

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
};

enum class BlockReferenceKind {
  kData,
  kIndirect,
};

struct CheckIssue {
  IssueCode code{IssueCode::kRootInodeMarkedFree};
  std::uint32_t inode_number{0};
  std::uint32_t block_number{0};
  std::uint32_t related_inode_number{0};
  std::string detail;
  std::uint64_t first_bitmap_bit{0};
  std::uint64_t occurrence_count{1};
};

struct DiscoveredInode {
  std::uint32_t inode_number{0};
  bool bitmap_allocated{false};
  ondisk::InodeRecord inode;
};

struct DirectoryEdge {
  std::uint32_t parent_inode_number{0};
  std::uint32_t child_inode_number{0};
};

struct DirectoryScanState {
  std::uint32_t inode_number{0};
  bool complete{false};
};

struct BlockReference {
  std::uint32_t block_number{0};
  std::uint32_t inode_number{0};
  std::uint32_t logical_block{0};
  BlockReferenceKind kind{BlockReferenceKind::kData};
};

struct InodeLinkObservation {
  std::uint32_t inode_number{0};
  std::uint32_t declared_link_count{0};
  std::uint64_t observed_dentry_references{0};
  std::uint64_t observed_child_directories{0};
  std::uint64_t expected_link_count{0};
  bool expectation_complete{false};
};

struct ConsistencyReport {
  ScanStatus status{ScanStatus::kAborted};
  bool bitmap_geometry_scan_complete{false};
  bool root_inode_decoded{false};
  bool root_reachability_complete{false};
  bool block_reference_scan_complete{false};
  bool inode_reference_scan_complete{false};
  ondisk::InodeRecord root_inode;
  std::vector<ondisk::DirectoryEntry> root_entries;
  std::vector<DiscoveredInode> discovered_inodes;
  std::vector<DirectoryEdge> directory_edges;
  std::vector<DirectoryScanState> directory_scan_states;
  std::vector<BlockReference> block_references;
  std::vector<InodeLinkObservation> inode_link_observations;
  std::vector<std::uint32_t> allocated_unreferenced_data_blocks;
  std::vector<std::uint32_t> physical_scan_inode_numbers;
  std::vector<std::uint32_t> root_reachable_inode_numbers;
  std::vector<CheckIssue> issues;
};

// Requires a stable, clean journal view; then scans physical inode evidence,
// builds the directory graph, and derives namespace reachability separately.
int CheckImage(const std::string& image_path, ConsistencyReport* output,
               std::string* detail);

}  // namespace eufs::checker
