// 验证 mkfs 生成的 superblock、固定区域、bitmap、根 inode、根目录和双 control 基线。
// 这是所有镜像级测试共享的格式来源，不能依赖手工拼出的“看似合法”镜像。
#include "checker/consistency_checker.h"
#include "journal/ondisk_journal.h"
#include "metadata/ondisk_format.h"
#include "object/request_ledger_format.h"
#include "storage/mkfs.h"
#include "storage/image_reader.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

bool PreadAll(int fd, std::uint8_t* output, std::size_t size,
              std::uint64_t offset) {
  std::size_t read_bytes = 0;
  while (read_bytes < size) {
    const auto result = pread(fd, output + read_bytes, size - read_bytes,
                              static_cast<off_t>(offset + read_bytes));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    read_bytes += static_cast<std::size_t>(result);
  }
  return true;
}

bool BitmapBit(const std::vector<std::uint8_t>& bitmap, std::uint32_t bit) {
  return (bitmap[bit / 8U] & static_cast<std::uint8_t>(1U << (bit % 8U))) !=
         0;
}

std::uint32_t GetLe32(const std::uint8_t* input) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<std::uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-mkfs-test-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  // 13 个 ledger 数据块强制覆盖 direct -> single-indirect 边界。
  options.request_ledger_entries = 13U * 32U;

  eufs::ondisk::Superblock formatted;
  std::string error;
  Require(eufs::storage::FormatImage(options, &formatted, &error),
          error.c_str());

  const int image_fd = open(options.image_path.c_str(), O_RDONLY | O_CLOEXEC);
  Require(image_fd >= 0, "formatted image could not be opened");
  struct stat image_stat {};
  Require(fstat(image_fd, &image_stat) == 0 &&
              static_cast<std::uint64_t>(image_stat.st_size) ==
                  options.image_size_bytes,
          "formatted image size is incorrect");

  eufs::ondisk::Block superblock_bytes{};
  Require(PreadAll(image_fd, superblock_bytes.data(), superblock_bytes.size(),
                   0),
          "superblock read failed");
  eufs::ondisk::Superblock decoded;
  Require(eufs::ondisk::DecodeSuperblock(superblock_bytes, &decoded, &error),
          error.c_str());
  Require(decoded.total_blocks == 2048 && decoded.total_inodes == 128 &&
              decoded.root_inode == 1 &&
              decoded.feature_incompat ==
                  eufs::ondisk::kFeatureIncompatRequestLedger,
          "decoded mkfs geometry is incorrect");

  std::vector<std::uint8_t> inode_bitmap(
      decoded.inode_bitmap.block_count * eufs::ondisk::kBlockSize);
  Require(PreadAll(image_fd, inode_bitmap.data(), inode_bitmap.size(),
                   static_cast<std::uint64_t>(
                       decoded.inode_bitmap.start_block) *
                       eufs::ondisk::kBlockSize),
          "inode bitmap read failed");
  Require(BitmapBit(inode_bitmap, 0), "root inode is not allocated");
  Require(BitmapBit(inode_bitmap, 1), "request ledger inode is not allocated");
  Require(!BitmapBit(inode_bitmap, 2), "first user inode is unexpectedly allocated");
  Require(BitmapBit(inode_bitmap, decoded.total_inodes),
          "inode bitmap tail is not reserved");

  std::vector<std::uint8_t> block_bitmap(
      decoded.block_bitmap.block_count * eufs::ondisk::kBlockSize);
  Require(PreadAll(image_fd, block_bitmap.data(), block_bitmap.size(),
                   static_cast<std::uint64_t>(
                       decoded.block_bitmap.start_block) *
                       eufs::ondisk::kBlockSize),
          "block bitmap read failed");
  Require(BitmapBit(block_bitmap, decoded.data.start_block - 1),
          "metadata block is not reserved in block bitmap");
  const std::uint32_t root_directory_block = decoded.data.start_block;
  const std::uint32_t first_ledger_block = root_directory_block + 1U;
  const std::uint32_t ledger_indirect_block = first_ledger_block + 13U;
  for (std::uint32_t block = root_directory_block;
       block <= ledger_indirect_block; ++block) {
    Require(BitmapBit(block_bitmap, block),
            "request ledger physical block is not allocated");
  }
  Require(!BitmapBit(block_bitmap, ledger_indirect_block + 1U),
          "first user data block is unexpectedly allocated");
  Require(BitmapBit(block_bitmap, decoded.total_blocks),
          "block bitmap tail is not reserved");

  eufs::ondisk::InodeBytes root_bytes{};
  Require(PreadAll(image_fd, root_bytes.data(), root_bytes.size(),
                   static_cast<std::uint64_t>(decoded.inode_table.start_block) *
                       eufs::ondisk::kBlockSize),
          "root inode read failed");
  eufs::ondisk::InodeRecord root;
  Require(eufs::ondisk::DecodeInode(root_bytes, 1, &root, &error),
          error.c_str());
  Require(S_ISDIR(root.mode) && root.link_count == 2 &&
              root.size == eufs::ondisk::kBlockSize &&
              root.direct_blocks[0] == root_directory_block &&
              root.indirect_block == 0,
          "root inode fields are incorrect");

  eufs::ondisk::InodeBytes ledger_inode_bytes{};
  Require(PreadAll(
              image_fd, ledger_inode_bytes.data(), ledger_inode_bytes.size(),
              static_cast<std::uint64_t>(decoded.inode_table.start_block) *
                      eufs::ondisk::kBlockSize +
                  eufs::ondisk::kInodeRecordSize),
          "request ledger inode read failed");
  eufs::ondisk::InodeRecord ledger_inode;
  Require(eufs::ondisk::DecodeInode(ledger_inode_bytes, 2, &ledger_inode,
                                    &error) &&
              S_ISREG(ledger_inode.mode) && ledger_inode.link_count == 1 &&
              ledger_inode.size == 13ULL * eufs::ondisk::kBlockSize &&
              ledger_inode.direct_blocks[0] == first_ledger_block &&
              ledger_inode.direct_blocks[11] == first_ledger_block + 11U &&
              ledger_inode.indirect_block == ledger_indirect_block,
          "request ledger inode mapping is incorrect");

  eufs::ondisk::Block root_directory{};
  Require(PreadAll(image_fd, root_directory.data(), root_directory.size(),
                   static_cast<std::uint64_t>(root_directory_block) *
                       eufs::ondisk::kBlockSize),
          "request ledger directory entry read failed");
  eufs::ondisk::DirectoryEntry ledger_entry;
  Require(eufs::ondisk::DecodeDirectoryEntry(
              root_directory.data(), root_directory.size(), &ledger_entry,
              &error) &&
              ledger_entry.inode == 2 &&
              ledger_entry.name == eufs::object_store::kRequestLedgerName &&
              ledger_entry.record_length == eufs::ondisk::kBlockSize,
          "request ledger directory entry is incorrect");

  eufs::ondisk::Block indirect{};
  Require(PreadAll(image_fd, indirect.data(), indirect.size(),
                   static_cast<std::uint64_t>(ledger_indirect_block) *
                       eufs::ondisk::kBlockSize),
          "request ledger indirect block read failed");
  Require(GetLe32(indirect.data()) == first_ledger_block + 12U &&
              std::all_of(indirect.begin() + 4, indirect.end(),
                          [](std::uint8_t byte) { return byte == 0; }),
          "request ledger indirect mapping or reserved tail is incorrect");

  for (std::uint32_t block = first_ledger_block;
       block < first_ledger_block + 13U; ++block) {
    eufs::ondisk::Block ledger_data{};
    ledger_data.fill(0xa5);
    Require(PreadAll(image_fd, ledger_data.data(), ledger_data.size(),
                     static_cast<std::uint64_t>(block) *
                         eufs::ondisk::kBlockSize) &&
                std::all_of(ledger_data.begin(), ledger_data.end(),
                            [](std::uint8_t byte) { return byte == 0; }),
            "request ledger slot block is not fully zeroed");
  }

  eufs::ondisk::Block control_a_bytes{};
  eufs::ondisk::Block control_b_bytes{};
  const std::uint64_t control_a_offset =
      static_cast<std::uint64_t>(decoded.journal.start_block) *
      eufs::ondisk::kBlockSize;
  Require(PreadAll(image_fd, control_a_bytes.data(), control_a_bytes.size(),
                   control_a_offset) &&
              PreadAll(image_fd, control_b_bytes.data(), control_b_bytes.size(),
                       control_a_offset + eufs::ondisk::kBlockSize),
          "journal controls could not be read from the formatted image");
  Require(std::equal(control_a_bytes.begin(), control_a_bytes.end(),
                     control_b_bytes.begin()),
          "initial journal control copies are not identical");
  eufs::journal::JournalControl control_a;
  eufs::journal::JournalControl control_b;
  Require(eufs::journal::DecodeControl(control_a_bytes, &control_a, &error) &&
              eufs::journal::DecodeControl(control_b_bytes, &control_b, &error),
          error.c_str());
  Require(control_a.filesystem_uuid == decoded.filesystem_uuid &&
              control_b.filesystem_uuid == decoded.filesystem_uuid &&
              control_a.ring_blocks == decoded.journal.block_count - 2U &&
              control_a.generation == 0 && control_a.head == 0 &&
              control_a.tail == 0 && control_a.used_blocks == 0 &&
              control_a.next_transaction_id == 1,
          "initial journal control state is incorrect");
  close(image_fd);

  // 不依赖 mkfs 的原始偏移假设，让正式 reader 重新验证路径、inode 和间接映射。
  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &error) == 0,
          error.c_str());
  std::uint32_t resolved_inode = 0;
  eufs::ondisk::InodeRecord resolved_ledger;
  const std::string ledger_path =
      std::string("/") + std::string(eufs::object_store::kRequestLedgerName);
  Require(reader->ResolvePath(ledger_path, &resolved_inode, &resolved_ledger,
                              &error) == 0 &&
              resolved_inode == eufs::object_store::kRequestLedgerInodeNumber &&
              resolved_ledger.size == 13ULL * eufs::ondisk::kBlockSize,
          "ImageReader could not resolve the request ledger identity");
  std::array<std::uint8_t, 2> boundary_bytes{0xa5, 0xa5};
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(
              resolved_inode, 12ULL * eufs::ondisk::kBlockSize - 1U,
              boundary_bytes.data(), boundary_bytes.size(), &bytes_read,
              &error) == 0 &&
              bytes_read == boundary_bytes.size() && boundary_bytes[0] == 0 &&
              boundary_bytes[1] == 0,
          "ImageReader could not cross the ledger direct/indirect boundary");
  reader.reset();

  // eufsck 必须把系统文件判定为根可达、块有引用且 bitmap 一致。
  eufs::checker::ConsistencyReport report;
  Require(eufs::checker::CheckImage(options.image_path, &report, &error) == 0 &&
              report.status == eufs::checker::ScanStatus::kComplete &&
              report.issues.empty(),
          "eufsck rejected the preallocated request ledger image");

  Require(!eufs::storage::FormatImage(options, nullptr, &error),
          "mkfs overwrote an existing image without --force");
  options.force = true;
  Require(eufs::storage::FormatImage(options, nullptr, &error),
          "mkfs --force could not replace the image");

  // 非整块记录数在打开目标路径前就应拒绝，不能留下部分镜像。
  const std::string invalid_path = options.image_path + ".invalid-ledger";
  unlink(invalid_path.c_str());
  options.image_path = invalid_path;
  options.force = false;
  options.request_ledger_entries = 33;
  Require(!eufs::storage::FormatImage(options, nullptr, &error) &&
              access(invalid_path.c_str(), F_OK) != 0,
          "mkfs accepted a partial ledger block or left a partial image");

  unlink(path_template.data());
  std::cout << "PASS: eufs-mkfs image layout test\n";
  return 0;
}
