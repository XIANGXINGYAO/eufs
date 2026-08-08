// 自检 tests/support 中的直接落盘辅助器能否忠实应用 planner 生成的块镜像。
// 该辅助器只构造测试前置状态，不参与 eufsd 产品链接。
#include "metadata/empty_file_create_plan.h"
#include "storage/image_reader.h"
#include "storage/mkfs.h"
#include "tests/support/writable_image.h"

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
  std::strcpy(path_template.data(), "/tmp/eufs-writable-image-XXXXXX");
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
  eufs::metadata::EmptyFileCreatePlan plan;
  Require(eufs::metadata::PrepareRootEmptyFileCreate(
              *reader, "a.txt", 0644, 1000, 1000, 123456789ULL, &plan,
              &detail) == 0,
          detail.c_str());

  Require(eufs::storage::ApplyCreatePlan(options.image_path, plan, &detail) ==
              -EBUSY,
          "writer ignored the reader's shared image lock");
  reader.reset();

  Require(eufs::storage::ApplyCreatePlan(options.image_path, plan, &detail) ==
              0,
          detail.c_str());
  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  std::uint32_t inode_number = 0;
  eufs::ondisk::InodeRecord inode;
  Require(reader->ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0 &&
              inode_number == 2 && S_ISREG(inode.mode) && inode.size == 0,
          "reopened image does not contain the planned empty file");
  reader.reset();

  Require(eufs::storage::ApplyCreatePlan(options.image_path, plan, &detail) ==
              -ESTALE,
          "stale create plan unexpectedly overwrote newer home blocks");

  Require(eufs::storage::ImageReader::Open(options.image_path, &reader,
                                           &detail) == 0,
          detail.c_str());
  Require(reader->ResolvePath("/a.txt", &inode_number, &inode, &detail) == 0,
          "stale-plan rejection damaged the persisted file");

  std::cout << "persisted_path=/a.txt inode=" << inode_number
            << " size=" << inode.size << '\n';
  std::cout << "stale_plan_result=" << -ESTALE << '\n';
  reader.reset();
  unlink(options.image_path.c_str());
  std::cout << "PASS: writable image normal create persistence test\n";
  return 0;
}
