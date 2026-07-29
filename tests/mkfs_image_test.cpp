#include "journal/ondisk_journal.h"
#include "metadata/ondisk_format.h"
#include "storage/mkfs.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
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
              decoded.root_inode == 1,
          "decoded mkfs geometry is incorrect");

  std::vector<std::uint8_t> inode_bitmap(
      decoded.inode_bitmap.block_count * eufs::ondisk::kBlockSize);
  Require(PreadAll(image_fd, inode_bitmap.data(), inode_bitmap.size(),
                   static_cast<std::uint64_t>(
                       decoded.inode_bitmap.start_block) *
                       eufs::ondisk::kBlockSize),
          "inode bitmap read failed");
  Require(BitmapBit(inode_bitmap, 0), "root inode is not allocated");
  Require(!BitmapBit(inode_bitmap, 1), "second inode is unexpectedly allocated");
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
  Require(!BitmapBit(block_bitmap, decoded.data.start_block),
          "first data block is unexpectedly allocated");
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
  Require(S_ISDIR(root.mode) && root.link_count == 2 && root.size == 0 &&
              root.indirect_block == 0,
          "root inode fields are incorrect");

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

  Require(!eufs::storage::FormatImage(options, nullptr, &error),
          "mkfs overwrote an existing image without --force");
  options.force = true;
  Require(eufs::storage::FormatImage(options, nullptr, &error),
          "mkfs --force could not replace the image");

  unlink(options.image_path.c_str());
  std::cout << "PASS: eufs-mkfs image layout test\n";
  return 0;
}
