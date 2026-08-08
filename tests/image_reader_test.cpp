// 验证 ImageReader 对 superblock、control、bitmap、inode、目录和文件内容的只读解析契约。
// 损坏输入必须返回明确错误，不能越界、短读后继续解析或接受不支持的格式特性。
#include "metadata/ondisk_format.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"

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
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

bool PwriteAll(int fd, const std::uint8_t* input, std::size_t size,
               std::uint64_t offset) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pwrite(fd, input + completed, size - completed,
                               static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    completed += static_cast<std::size_t>(result);
  }
  return true;
}

void PutLe32(std::uint8_t* output, std::uint32_t value) {
  for (std::size_t index = 0; index < 4; ++index) {
    output[index] = static_cast<std::uint8_t>(value >> (index * 8U));
  }
}

void SetBitmapBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit) {
  (*bitmap)[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

std::string BuildFixture() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-reader-test-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  eufs::ondisk::Superblock superblock;
  std::string error;
  Require(eufs::storage::FormatImage(options, &superblock, &error),
          error.c_str());

  const int fd = open(options.image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "fixture image open failed");
  const std::uint32_t root_directory_block = superblock.data.start_block;
  const std::uint32_t hello_data_block = root_directory_block + 1;
  const std::uint32_t big_direct_start = root_directory_block + 2;
  const std::uint32_t big_indirect_block = root_directory_block + 14;
  const std::uint32_t big_extra_block = root_directory_block + 15;

  std::vector<std::uint8_t> inode_bitmap(
      superblock.inode_bitmap.block_count * eufs::ondisk::kBlockSize);
  Require(PreadAll(fd, inode_bitmap.data(), inode_bitmap.size(),
                   static_cast<std::uint64_t>(
                       superblock.inode_bitmap.start_block) *
                       eufs::ondisk::kBlockSize),
          "fixture inode bitmap read failed");
  SetBitmapBit(&inode_bitmap, 1);
  SetBitmapBit(&inode_bitmap, 2);
  Require(PwriteAll(fd, inode_bitmap.data(), inode_bitmap.size(),
                    static_cast<std::uint64_t>(
                        superblock.inode_bitmap.start_block) *
                        eufs::ondisk::kBlockSize),
          "fixture inode bitmap write failed");

  std::vector<std::uint8_t> block_bitmap(
      superblock.block_bitmap.block_count * eufs::ondisk::kBlockSize);
  Require(PreadAll(fd, block_bitmap.data(), block_bitmap.size(),
                   static_cast<std::uint64_t>(
                       superblock.block_bitmap.start_block) *
                       eufs::ondisk::kBlockSize),
          "fixture block bitmap read failed");
  for (std::uint32_t block = root_directory_block;
       block <= big_extra_block; ++block) {
    SetBitmapBit(&block_bitmap, block);
  }
  Require(PwriteAll(fd, block_bitmap.data(), block_bitmap.size(),
                    static_cast<std::uint64_t>(
                        superblock.block_bitmap.start_block) *
                        eufs::ondisk::kBlockSize),
          "fixture block bitmap write failed");

  eufs::ondisk::InodeBytes root_bytes{};
  const std::uint64_t inode_table_offset =
      static_cast<std::uint64_t>(superblock.inode_table.start_block) *
      eufs::ondisk::kBlockSize;
  Require(PreadAll(fd, root_bytes.data(), root_bytes.size(), inode_table_offset),
          "fixture root inode read failed");
  eufs::ondisk::InodeRecord root;
  Require(eufs::ondisk::DecodeInode(root_bytes, 1, &root, &error),
          error.c_str());
  root.size = eufs::ondisk::kBlockSize;
  root.direct_blocks[0] = root_directory_block;
  Require(eufs::ondisk::EncodeInode(root, &root_bytes, &error), error.c_str());
  Require(PwriteAll(fd, root_bytes.data(), root_bytes.size(), inode_table_offset),
          "fixture root inode write failed");

  eufs::ondisk::InodeRecord hello;
  hello.inode_number = 2;
  hello.mode = S_IFREG | 0444;
  hello.uid = root.uid;
  hello.gid = root.gid;
  hello.link_count = 1;
  hello.size = 17;
  hello.generation = 1;
  hello.direct_blocks[0] = hello_data_block;
  eufs::ondisk::InodeBytes hello_bytes{};
  Require(eufs::ondisk::EncodeInode(hello, &hello_bytes, &error),
          error.c_str());
  Require(PwriteAll(fd, hello_bytes.data(), hello_bytes.size(),
                    inode_table_offset + eufs::ondisk::kInodeRecordSize),
          "fixture hello inode write failed");

  eufs::ondisk::InodeRecord big;
  big.inode_number = 3;
  big.mode = S_IFREG | 0444;
  big.uid = root.uid;
  big.gid = root.gid;
  big.link_count = 1;
  big.size = eufs::ondisk::kDirectBlockCount * eufs::ondisk::kBlockSize + 5;
  big.generation = 1;
  for (std::size_t index = 0; index < big.direct_blocks.size(); ++index) {
    big.direct_blocks[index] =
        big_direct_start + static_cast<std::uint32_t>(index);
  }
  big.indirect_block = big_indirect_block;
  eufs::ondisk::InodeBytes big_bytes{};
  Require(eufs::ondisk::EncodeInode(big, &big_bytes, &error), error.c_str());
  Require(PwriteAll(fd, big_bytes.data(), big_bytes.size(),
                    inode_table_offset + 2 * eufs::ondisk::kInodeRecordSize),
          "fixture big inode write failed");

  eufs::ondisk::Block directory_block{};
  eufs::ondisk::DirectoryEntry hello_entry{
      2, eufs::ondisk::DirectoryFileType::kRegular, "hello.txt", 0};
  const auto hello_record_length = static_cast<std::uint16_t>(
      eufs::ondisk::MinimumDirectoryRecordLength(hello_entry.name.size()));
  Require(eufs::ondisk::EncodeDirectoryEntry(
              hello_entry, hello_record_length, directory_block.data(),
              directory_block.size(), &error),
          error.c_str());
  eufs::ondisk::DirectoryEntry big_entry{
      3, eufs::ondisk::DirectoryFileType::kRegular, "big.bin", 0};
  Require(eufs::ondisk::EncodeDirectoryEntry(
              big_entry,
              static_cast<std::uint16_t>(directory_block.size() -
                                         hello_record_length),
              directory_block.data() + hello_record_length,
              directory_block.size() - hello_record_length, &error),
          error.c_str());
  Require(PwriteAll(fd, directory_block.data(), directory_block.size(),
                    static_cast<std::uint64_t>(root_directory_block) *
                        eufs::ondisk::kBlockSize),
          "fixture directory block write failed");

  constexpr std::string_view kHello = "hello from image\n";
  Require(PwriteAll(fd, reinterpret_cast<const std::uint8_t*>(kHello.data()),
                    kHello.size(),
                    static_cast<std::uint64_t>(hello_data_block) *
                        eufs::ondisk::kBlockSize),
          "fixture hello data write failed");
  for (std::size_t index = 0; index < big.direct_blocks.size(); ++index) {
    eufs::ondisk::Block block{};
    block.fill(static_cast<std::uint8_t>('A' + index));
    Require(PwriteAll(fd, block.data(), block.size(),
                      static_cast<std::uint64_t>(big.direct_blocks[index]) *
                          eufs::ondisk::kBlockSize),
            "fixture big direct data write failed");
  }
  eufs::ondisk::Block indirect{};
  PutLe32(indirect.data(), big_extra_block);
  Require(PwriteAll(fd, indirect.data(), indirect.size(),
                    static_cast<std::uint64_t>(big_indirect_block) *
                        eufs::ondisk::kBlockSize),
          "fixture indirect block write failed");
  constexpr std::array<std::uint8_t, 5> kExtra{'Z', 'Z', 'Z', 'Z', 'Z'};
  Require(PwriteAll(fd, kExtra.data(), kExtra.size(),
                    static_cast<std::uint64_t>(big_extra_block) *
                        eufs::ondisk::kBlockSize),
          "fixture extra data write failed");
  Require(fdatasync(fd) == 0, "fixture fdatasync failed");
  close(fd);
  return options.image_path;
}

}  // namespace

int main() {
  const std::string image_path = BuildFixture();
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(image_path, &reader, &detail) == 0,
          detail.c_str());

  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  Require(reader->ResolvePath("/hello.txt", &inode_number, &inode, &detail) ==
              0 &&
              inode_number == 2 && inode.size == 17,
          "hello path did not resolve to inode 2");
  std::array<std::uint8_t, 32> hello{};
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(inode_number, 0, hello.data(), hello.size(),
                           &bytes_read, &detail) == 0 &&
              std::string(reinterpret_cast<char*>(hello.data()), bytes_read) ==
                  "hello from image\n",
          "hello file data was not read from the image");

  std::vector<eufs::ondisk::DirectoryEntry> entries;
  Require(reader->ListDirectory(1, &entries, &detail) == 0 &&
              entries.size() == 2 && entries[0].name == "hello.txt" &&
              entries[1].name == "big.bin",
          "root directory entries were not decoded in order");

  Require(reader->ResolvePath("/big.bin", &inode_number, &inode, &detail) == 0 &&
              inode_number == 3,
          "big file path did not resolve");
  std::array<std::uint8_t, 7> boundary{};
  Require(reader->ReadFile(
              inode_number,
              eufs::ondisk::kDirectBlockCount * eufs::ondisk::kBlockSize - 2,
              boundary.data(), boundary.size(), &bytes_read, &detail) == 0 &&
              bytes_read == boundary.size() &&
              std::string(reinterpret_cast<char*>(boundary.data()),
                          boundary.size()) == "LLZZZZZ",
          "direct to single-indirect boundary read is incorrect");

  Require(reader->ResolvePath("/missing", &inode_number, &inode, &detail) ==
              -ENOENT,
          "missing path did not return ENOENT");

  eufs::storage::MkfsOptions locked_mkfs;
  locked_mkfs.image_path = image_path;
  locked_mkfs.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  locked_mkfs.total_inodes = 128;
  locked_mkfs.journal_blocks = 16;
  locked_mkfs.force = true;
  Require(!eufs::storage::FormatImage(locked_mkfs, nullptr, &detail),
          "mkfs --force truncated an image held by a reader lock");

  const std::uint32_t root_directory_block =
      reader->superblock().data.start_block;
  reader.reset();

  const int corrupt_fd = open(image_path.c_str(), O_RDWR | O_CLOEXEC);
  Require(corrupt_fd >= 0, "corrupt fixture open failed");
  const std::array<std::uint8_t, 2> bad_record_length{10, 0};
  Require(PwriteAll(
              corrupt_fd, bad_record_length.data(), bad_record_length.size(),
              static_cast<std::uint64_t>(root_directory_block) *
                      eufs::ondisk::kBlockSize +
                  4),
          "directory corruption write failed");
  Require(fdatasync(corrupt_fd) == 0, "directory corruption sync failed");
  close(corrupt_fd);
  Require(eufs::storage::ImageReader::Open(image_path, &reader, &detail) == 0,
          detail.c_str());
  Require(reader->ListDirectory(1, &entries, &detail) == -EUCLEAN,
          "malformed directory record did not return EUCLEAN");

  reader.reset();
  unlink(image_path.c_str());
  std::cout << "PASS: disk image reader and block mapping test\n";
  return 0;
}
