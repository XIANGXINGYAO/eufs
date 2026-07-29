#include "checker/consistency_checker.h"

#include "journal/ondisk_journal.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace eufs::checker {
namespace {

class UniqueFd {
 public:
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  int get() const { return fd_; }

 private:
  int fd_;
};

void SetDetail(std::string* detail, std::string_view value) {
  if (detail != nullptr) {
    detail->assign(value);
  }
}

void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

int PreadAll(int fd, std::uint8_t* output, std::size_t size,
             std::uint64_t offset, std::string* detail) {
  if (offset > static_cast<std::uint64_t>(
                   std::numeric_limits<off_t>::max()) ||
      size > static_cast<std::uint64_t>(
                 std::numeric_limits<off_t>::max()) - offset) {
    SetDetail(detail, "检查器读取偏移无法表示");
    return -EOVERFLOW;
  }
  std::size_t completed = 0;
  while (completed < size) {
    const auto result =
        pread(fd, output + completed, size - completed,
              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, "读取检查镜像", error_number);
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

bool BitmapBit(const std::vector<std::uint8_t>& bitmap, std::uint32_t bit) {
  return (bitmap[bit / 8U] &
          static_cast<std::uint8_t>(1U << (bit % 8U))) != 0;
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

void AddIssue(ConsistencyReport* report, IssueCode code,
              std::uint32_t inode_number, std::uint32_t block_number,
              std::uint32_t related_inode_number, std::string_view detail) {
  report->issues.push_back(CheckIssue{code, inode_number, block_number,
                                      related_inode_number,
                                      std::string(detail)});
}

int ReadRawInode(int fd, const ondisk::Superblock& superblock,
                 std::uint32_t inode_number, ondisk::InodeRecord* output,
                 std::string* detail) {
  if (inode_number == 0 || inode_number > superblock.total_inodes ||
      output == nullptr) {
    return -EINVAL;
  }
  const std::uint64_t inode_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
          ondisk::kBlockSize +
      static_cast<std::uint64_t>(inode_number - 1U) *
          ondisk::kInodeRecordSize;
  ondisk::InodeBytes inode_bytes{};
  int result = PreadAll(fd, inode_bytes.data(), inode_bytes.size(), inode_offset,
                        detail);
  if (result != 0) {
    return result;
  }
  if (!ondisk::DecodeInode(inode_bytes, inode_number, output, detail)) {
    return -EUCLEAN;
  }
  return 0;
}

enum class DirectoryVisitState {
  kUnseen,
  kActive,
  kDone,
};

struct DirectoryDfsFrame {
  std::uint32_t inode_number{0};
  std::size_t next_edge{0};
};

struct DirectoryEntryObservation {
  std::uint32_t parent_inode_number{0};
  std::uint32_t block_number{0};
  ondisk::DirectoryEntry entry;
};

struct ClearBitSummary {
  std::uint64_t first_bit{0};
  std::uint64_t count{0};
};

std::optional<ClearBitSummary> SummarizeClearBits(
    const std::vector<std::uint8_t>& bitmap, std::uint64_t begin,
    std::uint64_t end) {
  ClearBitSummary summary;
  for (std::uint64_t bit = begin; bit < end; ++bit) {
    if (BitmapBit(bitmap, static_cast<std::uint32_t>(bit))) {
      continue;
    }
    if (summary.count == 0) {
      summary.first_bit = bit;
    }
    ++summary.count;
  }
  return summary.count == 0 ? std::nullopt
                            : std::optional<ClearBitSummary>(summary);
}

void AddBitmapIssue(ConsistencyReport* report, IssueCode code,
                    std::uint32_t block_number,
                    const ClearBitSummary& summary, std::string_view detail) {
  CheckIssue issue{code, 0, block_number, 0, std::string(detail)};
  issue.first_bitmap_bit = summary.first_bit;
  issue.occurrence_count = summary.count;
  report->issues.push_back(std::move(issue));
}

bool DirectoryTypeMatchesMode(ondisk::DirectoryFileType type,
                              std::uint32_t mode) {
  if (type == ondisk::DirectoryFileType::kRegular) {
    return S_ISREG(mode);
  }
  if (type == ondisk::DirectoryFileType::kDirectory) {
    return S_ISDIR(mode);
  }
  return false;
}

}  // namespace

int CheckImage(const std::string& image_path, ConsistencyReport* output,
               std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (image_path.empty() || output == nullptr) {
    SetDetail(detail, "必须提供镜像路径和一致性报告输出");
    return -EINVAL;
  }
  *output = ConsistencyReport{};

  const int raw_fd = open(image_path.c_str(), O_RDONLY | O_CLOEXEC);
  if (raw_fd < 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "打开检查镜像", error_number);
    return -error_number;
  }
  UniqueFd fd(raw_fd);
  if (flock(fd.get(), LOCK_SH | LOCK_NB) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "锁定检查镜像", error_number);
    return error_number == EWOULDBLOCK ? -EBUSY : -error_number;
  }

  struct stat image_stat {};
  if (fstat(fd.get(), &image_stat) != 0 || image_stat.st_size < 0) {
    const int error_number = errno != 0 ? errno : EIO;
    SetSystemDetail(detail, "读取检查镜像属性", error_number);
    return -error_number;
  }

  ondisk::Block superblock_bytes{};
  int result = PreadAll(fd.get(), superblock_bytes.data(),
                        superblock_bytes.size(), 0, detail);
  if (result != 0) {
    return result;
  }
  ondisk::Superblock superblock;
  if (!ondisk::DecodeSuperblock(superblock_bytes, &superblock, detail)) {
    return -EUCLEAN;
  }
  const std::uint64_t expected_size =
      static_cast<std::uint64_t>(superblock.total_blocks) *
      ondisk::kBlockSize;
  if (static_cast<std::uint64_t>(image_stat.st_size) != expected_size) {
    SetDetail(detail, "镜像大小与 superblock 几何信息不一致");
    return -EUCLEAN;
  }

  ondisk::Block control_a{};
  ondisk::Block control_b{};
  const std::uint64_t control_a_offset =
      static_cast<std::uint64_t>(superblock.journal.start_block) *
      ondisk::kBlockSize;
  result = PreadAll(fd.get(), control_a.data(), control_a.size(),
                    control_a_offset, detail);
  if (result == 0) {
    result = PreadAll(fd.get(), control_b.data(), control_b.size(),
                      control_a_offset + ondisk::kBlockSize, detail);
  }
  if (result != 0) {
    return result;
  }
  journal::JournalControl journal_control;
  journal::ControlCopy selected_control{};
  const std::uint32_t expected_ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  if (!journal::SelectControl(control_a, control_b,
                              superblock.filesystem_uuid,
                              expected_ring_blocks, &journal_control,
                              &selected_control, detail)) {
    return -EUCLEAN;
  }
  static_cast<void>(selected_control);
  if (journal_control.used_blocks != 0) {
    AddIssue(output, IssueCode::kJournalRecoveryRequired,
             superblock.root_inode, 0, 0,
             "日志中存在已暴露事务；离线一致性检查前必须先运行 eufsd "
             "完成恢复");
    return 0;
  }

  std::vector<std::uint8_t> inode_bitmap(
      static_cast<std::size_t>(superblock.inode_bitmap.block_count) *
      ondisk::kBlockSize);
  result = PreadAll(
      fd.get(), inode_bitmap.data(), inode_bitmap.size(),
      static_cast<std::uint64_t>(superblock.inode_bitmap.start_block) *
          ondisk::kBlockSize,
      detail);
  if (result != 0) {
    return result;
  }

  std::vector<std::uint8_t> block_bitmap(
      static_cast<std::size_t>(superblock.block_bitmap.block_count) *
      ondisk::kBlockSize);
  result = PreadAll(
      fd.get(), block_bitmap.data(), block_bitmap.size(),
      static_cast<std::uint64_t>(superblock.block_bitmap.start_block) *
          ondisk::kBlockSize,
      detail);
  if (result != 0) {
    return result;
  }

  const auto missing_metadata_bits = SummarizeClearBits(
      block_bitmap, 0, superblock.data.start_block);
  if (missing_metadata_bits.has_value()) {
    AddBitmapIssue(
        output, IssueCode::kMetadataBlockMarkedFree,
        static_cast<std::uint32_t>(missing_metadata_bits->first_bit),
        *missing_metadata_bits,
        "block bitmap 的 metadata 保留区存在未置位的块");
  }

  const std::uint64_t inode_bitmap_capacity = inode_bitmap.size() * 8ULL;
  const auto free_inode_tail = SummarizeClearBits(
      inode_bitmap, superblock.total_inodes, inode_bitmap_capacity);
  if (free_inode_tail.has_value()) {
    AddBitmapIssue(output, IssueCode::kInodeBitmapTailNotReserved, 0,
                   *free_inode_tail,
                   "inode bitmap 超出 total_inodes 的尾部位未保留");
  }

  const std::uint64_t block_bitmap_capacity = block_bitmap.size() * 8ULL;
  const auto free_block_tail = SummarizeClearBits(
      block_bitmap, superblock.total_blocks, block_bitmap_capacity);
  if (free_block_tail.has_value()) {
    AddBitmapIssue(output, IssueCode::kBlockBitmapTailNotReserved, 0,
                   *free_block_tail,
                   "block bitmap 超出 total_blocks 的尾部位未保留");
  }
  output->bitmap_geometry_scan_complete = true;

  if (!BitmapBit(inode_bitmap, superblock.root_inode - 1U)) {
    AddIssue(output, IssueCode::kRootInodeMarkedFree, superblock.root_inode, 0,
             0, "superblock 指定的根 inode 在 inode 位图中未置位");
  }

  std::set<std::uint32_t> physical_scan;
  std::deque<std::uint32_t> physical_scan_queue;
  const auto enqueue_physical_scan = [&](std::uint32_t inode_number) {
    if (physical_scan.insert(inode_number).second) {
      physical_scan_queue.push_back(inode_number);
    }
  };
  enqueue_physical_scan(superblock.root_inode);
  for (std::uint32_t inode_number = 1;
       inode_number <= superblock.total_inodes; ++inode_number) {
    if (BitmapBit(inode_bitmap, inode_number - 1U)) {
      enqueue_physical_scan(inode_number);
    }
  }

  output->status = ScanStatus::kComplete;
  std::set<std::uint32_t> attempted_inodes;
  std::map<std::uint32_t, ondisk::InodeRecord> decoded_inodes;
  std::map<std::uint32_t, std::vector<std::uint32_t>> directory_edges;
  std::map<std::uint32_t, bool> directory_scan_complete;
  std::map<std::uint32_t, std::uint32_t> discovery_parent;
  std::map<std::uint32_t, std::uint32_t> discovery_block;
  std::map<std::uint32_t, std::set<std::string>> directory_active_names;
  std::vector<DirectoryEntryObservation> directory_entry_observations;
  std::map<std::uint32_t, BlockReference> first_block_references;
  std::map<std::uint32_t, std::vector<std::optional<std::uint32_t>>>
      inode_logical_blocks;
  bool block_reference_scan_complete = true;
  bool inode_reference_scan_complete = true;

  const auto register_block_reference =
      [&](std::uint32_t block_number, std::uint32_t inode_number,
          std::uint32_t logical_block, BlockReferenceKind kind,
          bool directory_data) {
        const BlockReference reference{block_number, inode_number,
                                       logical_block, kind};
        output->block_references.push_back(reference);
        if (block_number < superblock.data.start_block ||
            block_number >= superblock.total_blocks) {
          const IssueCode code =
              directory_data ? IssueCode::kDirectoryBlockOutOfRange
                             : IssueCode::kReferencedBlockOutOfRange;
          AddIssue(output, code, inode_number, block_number, 0,
                   "inode 块指针位于数据区之外");
          return false;
        }
        if (!BitmapBit(block_bitmap, block_number)) {
          const IssueCode code =
              directory_data ? IssueCode::kDirectoryBlockMarkedFree
                             : IssueCode::kReferencedBlockMarkedFree;
          AddIssue(output, code, inode_number, block_number, 0,
                   "被引用块在块位图中未置位");
        }
        const auto [first_it, inserted] =
            first_block_references.emplace(block_number, reference);
        if (!inserted) {
          AddIssue(output, IssueCode::kDuplicateBlockReference, inode_number,
                   block_number, first_it->second.inode_number,
                   "同一物理块存在多个 inode 映射引用");
        }
        return true;
      };

  while (!physical_scan_queue.empty()) {
    const std::uint32_t inode_number = physical_scan_queue.front();
    physical_scan_queue.pop_front();
    if (!attempted_inodes.insert(inode_number).second) {
      continue;
    }

    ondisk::InodeRecord inode;
    std::string inode_detail;
    result = ReadRawInode(fd.get(), superblock, inode_number, &inode,
                          &inode_detail);
    if (result != 0) {
      if (result != -EUCLEAN) {
        SetDetail(detail, inode_detail);
        return result;
      }
      if (inode_number == superblock.root_inode) {
        AddIssue(output, IssueCode::kRootInodeUndecodable, inode_number, 0, 0,
                 inode_detail);
      } else if (discovery_parent.count(inode_number) != 0) {
        AddIssue(output, IssueCode::kDentryTargetInodeUndecodable,
                 discovery_parent.at(inode_number),
                 discovery_block.at(inode_number), inode_number,
                 inode_detail);
      } else {
        AddIssue(output, IssueCode::kAllocatedInodeUndecodable, inode_number,
                 0, 0, inode_detail);
      }
      output->status = ScanStatus::kPartial;
      block_reference_scan_complete = false;
      inode_reference_scan_complete = false;
      continue;
    }

    decoded_inodes.emplace(inode_number, inode);
    const bool bitmap_allocated =
        BitmapBit(inode_bitmap, inode_number - 1U);
    if (inode_number == superblock.root_inode) {
      output->root_inode = inode;
      output->root_inode_decoded = true;
      if (!S_ISDIR(inode.mode)) {
        AddIssue(output, IssueCode::kRootInodeNotDirectory, inode_number, 0,
                 0, "superblock 指定的根 inode 实际不是目录");
        output->status = ScanStatus::kPartial;
      }
    } else {
      output->discovered_inodes.push_back(
          DiscoveredInode{inode_number, bitmap_allocated, inode});
    }

    const std::uint32_t required_blocks = static_cast<std::uint32_t>(
        (inode.size + ondisk::kBlockSize - 1U) / ondisk::kBlockSize);
    std::vector<std::optional<std::uint32_t>> logical_blocks(required_blocks);
    const std::uint32_t required_direct = std::min<std::uint32_t>(
        required_blocks, ondisk::kDirectBlockCount);
    for (std::uint32_t direct_index = 0;
         direct_index < ondisk::kDirectBlockCount; ++direct_index) {
      const std::uint32_t block = inode.direct_blocks[direct_index];
      const bool required = direct_index < required_direct;
      if (block == 0) {
        if (required) {
          AddIssue(output, IssueCode::kMissingRequiredBlock, inode_number, 0,
                   0, "inode 缺少文件大小要求的直接数据块");
        }
        continue;
      }
      if (!required) {
        AddIssue(output, IssueCode::kBlockPointerBeyondEnd, inode_number,
                 block, 0, "inode 在文件结尾之后仍有直接块指针");
      }
      const bool valid = register_block_reference(
          block, inode_number, direct_index, BlockReferenceKind::kData,
          S_ISDIR(inode.mode));
      if (required && valid) {
        logical_blocks[direct_index] = block;
      }
    }

    const std::uint32_t required_indirect =
        required_blocks > ondisk::kDirectBlockCount
            ? required_blocks - ondisk::kDirectBlockCount
            : 0;
    if (inode.indirect_block == 0) {
      if (required_indirect != 0) {
        AddIssue(output, IssueCode::kMissingRequiredBlock, inode_number, 0, 0,
                 "inode 缺少文件大小要求的间接索引块");
      }
    } else {
      if (required_indirect == 0) {
        AddIssue(output, IssueCode::kBlockPointerBeyondEnd, inode_number,
                 inode.indirect_block, 0,
                 "inode 在文件结尾之后仍有不必要的间接索引块");
      }
      const bool valid_indirect = register_block_reference(
          inode.indirect_block, inode_number, 0,
          BlockReferenceKind::kIndirect, false);
      if (!valid_indirect) {
        block_reference_scan_complete = false;
      } else {
        ondisk::Block indirect_block{};
        result = PreadAll(
            fd.get(), indirect_block.data(), indirect_block.size(),
            static_cast<std::uint64_t>(inode.indirect_block) *
                ondisk::kBlockSize,
            detail);
        if (result != 0) {
          return result;
        }
        const std::uint32_t indirect_entries =
            ondisk::kBlockSize / sizeof(std::uint32_t);
        for (std::uint32_t indirect_index = 0;
             indirect_index < indirect_entries; ++indirect_index) {
          const std::uint32_t block = GetLe32(
              indirect_block.data() +
              static_cast<std::size_t>(indirect_index) *
                  sizeof(std::uint32_t));
          const bool required = indirect_index < required_indirect;
          if (block == 0) {
            if (required) {
              AddIssue(output, IssueCode::kMissingRequiredBlock,
                       inode_number, 0, 0,
                       "inode 缺少文件大小要求的间接数据块");
            }
            continue;
          }
          const std::uint32_t logical_block =
              static_cast<std::uint32_t>(ondisk::kDirectBlockCount) +
              indirect_index;
          if (!required) {
            AddIssue(output, IssueCode::kBlockPointerBeyondEnd, inode_number,
                     block, 0,
                     "间接索引块在文件结尾之后仍有数据块指针");
          }
          const bool valid = register_block_reference(
              block, inode_number, logical_block, BlockReferenceKind::kData,
              S_ISDIR(inode.mode));
          if (required && valid) {
            logical_blocks[logical_block] = block;
          }
        }
      }
    }
    inode_logical_blocks.emplace(inode_number, std::move(logical_blocks));

    if (!S_ISDIR(inode.mode)) {
      continue;
    }

    bool this_directory_complete = true;
    if (inode.size % ondisk::kBlockSize != 0) {
      AddIssue(output, IssueCode::kDirectoryRecordUndecodable, inode_number,
               0, 0, "目录大小不是完整块大小的整数倍");
      output->status = ScanStatus::kPartial;
      directory_scan_complete[inode_number] = false;
      continue;
    }

    const std::uint32_t directory_blocks =
        static_cast<std::uint32_t>(inode.size / ondisk::kBlockSize);
    const auto mappings_it = inode_logical_blocks.find(inode_number);
    if (mappings_it == inode_logical_blocks.end() ||
        mappings_it->second.size() != directory_blocks) {
      AddIssue(output, IssueCode::kDirectoryRecordUndecodable, inode_number, 0,
               0, "无法取得完整的目录逻辑块映射证据");
      output->status = ScanStatus::kPartial;
      directory_scan_complete[inode_number] = false;
      continue;
    }
    for (std::uint32_t logical = 0; logical < directory_blocks; ++logical) {
      if (!mappings_it->second[logical].has_value()) {
        output->status = ScanStatus::kPartial;
        this_directory_complete = false;
        continue;
      }
      const std::uint32_t physical = *mappings_it->second[logical];

      ondisk::Block directory_block{};
      result = PreadAll(
          fd.get(), directory_block.data(), directory_block.size(),
          static_cast<std::uint64_t>(physical) * ondisk::kBlockSize, detail);
      if (result != 0) {
        return result;
      }
      std::size_t offset = 0;
      while (offset < directory_block.size()) {
        ondisk::DirectoryEntry entry;
        std::string entry_detail;
        if (!ondisk::DecodeDirectoryEntry(directory_block.data() + offset,
                                          directory_block.size() - offset,
                                          &entry, &entry_detail) ||
            entry.record_length == 0) {
          AddIssue(output, IssueCode::kDirectoryRecordUndecodable,
                   inode_number, physical, 0, entry_detail);
          output->status = ScanStatus::kPartial;
          this_directory_complete = false;
          break;
        }
        offset += entry.record_length;
        if (entry.inode == 0) {
          continue;
        }
        if (inode_number == superblock.root_inode) {
          output->root_entries.push_back(entry);
        }
        if (!directory_active_names[inode_number].insert(entry.name).second) {
          AddIssue(output, IssueCode::kDuplicateDentryName, inode_number,
                   physical, entry.inode,
                   "目录中存在重复的有效名称: " +
                       entry.name);
        }
        if (entry.inode > superblock.total_inodes) {
          AddIssue(output, IssueCode::kDentryTargetOutOfRange, inode_number,
                   physical, entry.inode,
                   "目录项目标位于 inode 表范围之外");
          output->status = ScanStatus::kPartial;
          continue;
        }

        directory_edges[inode_number].push_back(entry.inode);
        output->directory_edges.push_back(
            DirectoryEdge{inode_number, entry.inode});
        directory_entry_observations.push_back(
            DirectoryEntryObservation{inode_number, physical, entry});
        discovery_parent.emplace(entry.inode, inode_number);
        discovery_block.emplace(entry.inode, physical);
        enqueue_physical_scan(entry.inode);
        const bool allocated = BitmapBit(inode_bitmap, entry.inode - 1U);
        if (!allocated) {
          AddIssue(output, IssueCode::kDentryTargetInodeMarkedFree,
                   inode_number, physical, entry.inode,
                   "目录项目标在 inode 位图中未置位");
        }
      }
    }
    directory_scan_complete[inode_number] = this_directory_complete;
  }

  output->block_reference_scan_complete = block_reference_scan_complete;
  if (output->block_reference_scan_complete) {
    for (std::uint32_t block_number = superblock.data.start_block;
         block_number < superblock.total_blocks; ++block_number) {
      if (BitmapBit(block_bitmap, block_number) &&
          first_block_references.count(block_number) == 0) {
        output->allocated_unreferenced_data_blocks.push_back(block_number);
        AddIssue(output, IssueCode::kAllocatedDataBlockUnreferenced, 0,
                 block_number, 0,
                 "位图标记已分配的数据块没有任何 inode 映射引用");
      }
    }
  }

  for (const auto& [inode_number, complete] : directory_scan_complete) {
    static_cast<void>(inode_number);
    if (!complete) {
      inode_reference_scan_complete = false;
    }
  }

  std::map<std::uint32_t, std::uint64_t> observed_dentry_references;
  std::map<std::uint32_t, std::uint64_t> observed_child_directories;
  for (const DirectoryEntryObservation& observation :
       directory_entry_observations) {
    ++observed_dentry_references[observation.entry.inode];
    const auto child_it = decoded_inodes.find(observation.entry.inode);
    if (child_it == decoded_inodes.end()) {
      inode_reference_scan_complete = false;
    } else if (S_ISDIR(child_it->second.mode)) {
      ++observed_child_directories[observation.parent_inode_number];
    }
    if (observation.entry.inode == superblock.root_inode) {
      AddIssue(output, IssueCode::kRootDirectoryReferenced,
               superblock.root_inode, observation.block_number,
               observation.parent_inode_number,
               "磁盘目录项非法指向根 inode");
    }
  }

  output->inode_reference_scan_complete = inode_reference_scan_complete;
  for (const auto& [inode_number, inode] : decoded_inodes) {
    const std::uint64_t dentry_references =
        observed_dentry_references[inode_number];
    const std::uint64_t child_directories =
        observed_child_directories[inode_number];
    const bool expectation_complete =
        output->inode_reference_scan_complete &&
        (inode_number != superblock.root_inode || S_ISDIR(inode.mode));
    std::uint64_t expected_link_count = dentry_references;
    if (S_ISDIR(inode.mode)) {
      expected_link_count =
          inode_number == superblock.root_inode
              ? 2U + child_directories
              : 1U + dentry_references + child_directories;
    } else if (S_ISREG(inode.mode) && dentry_references > 1U) {
      AddIssue(output, IssueCode::kRegularFileHardLink, inode_number, 0, 0,
               "普通文件存在多个命名空间入口，但 eufs v1 不支持硬链接");
    }

    output->inode_link_observations.push_back(InodeLinkObservation{
        inode_number,
        inode.link_count,
        dentry_references,
        child_directories,
        expected_link_count,
        expectation_complete});
    if (expectation_complete && inode.link_count != expected_link_count) {
      AddIssue(output, IssueCode::kInodeLinkCountMismatch, inode_number, 0, 0,
               "inode link_count=" + std::to_string(inode.link_count) +
                   "，但命名空间证据要求 " +
                   std::to_string(expected_link_count));
    }
  }

  for (const DirectoryEntryObservation& observation :
       directory_entry_observations) {
    const auto child_it = decoded_inodes.find(observation.entry.inode);
    if (child_it == decoded_inodes.end()) {
      continue;
    }
    if (!DirectoryTypeMatchesMode(observation.entry.file_type,
                                  child_it->second.mode)) {
      AddIssue(output, IssueCode::kDentryTypeMismatch,
               observation.parent_inode_number, observation.block_number,
               observation.entry.inode,
               "目录项类型与目标 inode mode 不一致");
    }
  }

  std::map<std::uint32_t, DirectoryVisitState> directory_visit_state;
  const auto is_decoded_directory = [&](std::uint32_t inode_number) {
    const auto inode_it = decoded_inodes.find(inode_number);
    return inode_it != decoded_inodes.end() && S_ISDIR(inode_it->second.mode);
  };
  for (const auto& [start_inode, inode] : decoded_inodes) {
    if (!S_ISDIR(inode.mode) ||
        directory_visit_state[start_inode] != DirectoryVisitState::kUnseen) {
      continue;
    }

    std::vector<DirectoryDfsFrame> dfs_stack;
    directory_visit_state[start_inode] = DirectoryVisitState::kActive;
    dfs_stack.push_back(DirectoryDfsFrame{start_inode, 0});
    while (!dfs_stack.empty()) {
      DirectoryDfsFrame& frame = dfs_stack.back();
      const auto edges_it = directory_edges.find(frame.inode_number);
      if (edges_it == directory_edges.end() ||
          frame.next_edge >= edges_it->second.size()) {
        directory_visit_state[frame.inode_number] =
            DirectoryVisitState::kDone;
        dfs_stack.pop_back();
        continue;
      }

      const std::uint32_t child = edges_it->second[frame.next_edge++];
      if (!is_decoded_directory(child)) {
        continue;
      }
      const DirectoryVisitState child_state = directory_visit_state[child];
      if (child_state == DirectoryVisitState::kActive) {
        AddIssue(output, IssueCode::kDirectoryCycle, frame.inode_number, 0,
                 child,
                 "目录边指向 DFS 栈中仍处于 active 状态的祖先");
        continue;
      }
      if (child_state == DirectoryVisitState::kUnseen) {
        directory_visit_state[child] = DirectoryVisitState::kActive;
        dfs_stack.push_back(DirectoryDfsFrame{child, 0});
      }
    }
  }

  std::set<std::uint32_t> linked_directories;
  for (const DirectoryEdge& edge : output->directory_edges) {
    if (!is_decoded_directory(edge.child_inode_number)) {
      continue;
    }
    if (!linked_directories.insert(edge.child_inode_number).second) {
      AddIssue(output, IssueCode::kDirectoryHardLink,
               edge.parent_inode_number, 0, edge.child_inode_number,
               "目录 inode 存在多个命名空间入口");
    }
  }

  std::set<std::uint32_t> root_reachable;
  std::deque<std::uint32_t> reachability_queue;
  root_reachable.insert(superblock.root_inode);
  reachability_queue.push_back(superblock.root_inode);
  const auto decoded_root_it = decoded_inodes.find(superblock.root_inode);
  bool root_reachability_complete =
      decoded_root_it != decoded_inodes.end() &&
      S_ISDIR(decoded_root_it->second.mode);
  while (!reachability_queue.empty()) {
    const std::uint32_t inode_number = reachability_queue.front();
    reachability_queue.pop_front();
    const auto inode_it = decoded_inodes.find(inode_number);
    if (inode_it == decoded_inodes.end()) {
      root_reachability_complete = false;
      continue;
    }
    if (!S_ISDIR(inode_it->second.mode)) {
      continue;
    }
    const auto complete_it = directory_scan_complete.find(inode_number);
    if (complete_it == directory_scan_complete.end() ||
        !complete_it->second) {
      root_reachability_complete = false;
    }
    const auto edges_it = directory_edges.find(inode_number);
    if (edges_it == directory_edges.end()) {
      continue;
    }
    for (const std::uint32_t child : edges_it->second) {
      if (root_reachable.insert(child).second) {
        reachability_queue.push_back(child);
      }
    }
  }

  output->root_reachability_complete = root_reachability_complete;
  if (output->root_reachability_complete) {
    for (std::uint32_t inode_number = 1;
         inode_number <= superblock.total_inodes; ++inode_number) {
      if (inode_number != superblock.root_inode &&
          BitmapBit(inode_bitmap, inode_number - 1U) &&
          root_reachable.count(inode_number) == 0) {
        AddIssue(output, IssueCode::kInodeUnreachable, inode_number, 0, 0,
                 "已分配 inode 无法从根目录到达");
      }
    }
  }

  output->physical_scan_inode_numbers.assign(physical_scan.begin(),
                                              physical_scan.end());
  output->root_reachable_inode_numbers.assign(root_reachable.begin(),
                                               root_reachable.end());
  for (const auto& [inode_number, complete] : directory_scan_complete) {
    output->directory_scan_states.push_back(
        DirectoryScanState{inode_number, complete});
  }
  return 0;
}

}  // namespace eufs::checker
