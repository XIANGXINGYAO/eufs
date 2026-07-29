#include "metadata/empty_file_create_plan.h"
#include "metadata/first_block_write_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "storage/writable_image.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

}  // namespace

int main() {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-first-write-apply-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 64ULL * 1024ULL * 1024ULL;
  options.total_inodes = 1024;
  options.journal_blocks = 256;
  std::string detail;
  Require(eufs::storage::FormatImage(options, nullptr, &detail),
          detail.c_str());

  std::unique_ptr<eufs::storage::ImageReader> reader;
  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::EmptyFileCreatePlan create_plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 100ULL, &create_plan,
              &detail) == 0,
          detail.c_str());
  reader.reset();
  Require(eufs::storage::ApplyCreatePlan(options.image_path, create_plan,
                                         &detail) == 0,
          detail.c_str());

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  eufs::metadata::FirstBlockWritePlan write_plan;
  Require(eufs::metadata::PrepareFirstBlockWrite(
              *reader, 2, "hello", 200ULL, &write_plan, &detail) == 0,
          detail.c_str());
  Require(eufs::storage::ApplyFirstBlockWritePlan(
              options.image_path, write_plan, &detail) == -EBUSY,
          "first-block writer ignored the reader's shared image lock");
  reader.reset();

  Require(eufs::storage::ApplyFirstBlockWritePlan(
              options.image_path, write_plan, &detail) == 0,
          detail.c_str());
  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());

  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  Require(reader->ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0,
          detail.c_str());
  Require(inode_number == 2 && inode.size == 5 &&
              inode.direct_blocks[0] == 292 &&
              reader->IsBlockAllocated(292),
          "reopened inode or block bitmap does not describe persisted hello");

  std::array<std::uint8_t, 8> content{};
  std::size_t bytes_read = 0;
  Require(reader->ReadFile(inode_number, 0, content.data(), content.size(),
                           &bytes_read, &detail) == 0,
          detail.c_str());
  Require(bytes_read == 5 &&
              std::equal(content.begin(), content.begin() + bytes_read,
                         reinterpret_cast<const std::uint8_t*>("hello")),
          "reopened reader did not return the persisted hello bytes");
  reader.reset();

  Require(eufs::storage::ApplyFirstBlockWritePlan(
              options.image_path, write_plan, &detail) == -ESTALE,
          "stale first-block write plan overwrote newer home blocks");

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  content.fill(0);
  bytes_read = 0;
  Require(reader->ReadFile(2, 0, content.data(), content.size(), &bytes_read,
                           &detail) == 0 &&
              bytes_read == 5 &&
              std::equal(content.begin(), content.begin() + bytes_read,
                         reinterpret_cast<const std::uint8_t*>("hello")),
          "stale-plan rejection damaged the persisted file content");

  std::cout << "persisted_path=/a.txt inode=2 size=5 data=hello block=292\n";
  std::cout << "stale_plan_result=" << -ESTALE << '\n';
  reader.reset();
  unlink(options.image_path.c_str());
  std::cout << "PASS: first data-block normal persistence test\n";
  return 0;
}
