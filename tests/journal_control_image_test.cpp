#include "journal/ondisk_journal.h"
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
#include <unistd.h>

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

std::string CreateImage(eufs::ondisk::Superblock* superblock) {
  std::array<char, 64> path_template{};
  std::strcpy(path_template.data(), "/tmp/eufs-control-image-test-XXXXXX");
  const int temporary_fd = mkstemp(path_template.data());
  Require(temporary_fd >= 0, "mkstemp failed");
  close(temporary_fd);
  unlink(path_template.data());

  eufs::storage::MkfsOptions options;
  options.image_path = path_template.data();
  options.image_size_bytes = 8ULL * 1024ULL * 1024ULL;
  options.total_inodes = 128;
  options.journal_blocks = 16;
  std::string error;
  Require(eufs::storage::FormatImage(options, superblock, &error),
          error.c_str());
  return options.image_path;
}

std::uint64_t ControlOffset(const eufs::ondisk::Superblock& superblock,
                            eufs::journal::ControlCopy copy) {
  const std::uint32_t relative =
      copy == eufs::journal::ControlCopy::kA ? 0U : 1U;
  return static_cast<std::uint64_t>(superblock.journal.start_block + relative) *
         eufs::ondisk::kBlockSize;
}

void CorruptControl(const std::string& path,
                    const eufs::ondisk::Superblock& superblock,
                    eufs::journal::ControlCopy copy) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "control corruption open failed");
  eufs::ondisk::Block block{};
  const std::uint64_t offset = ControlOffset(superblock, copy);
  Require(PreadAll(fd, block.data(), block.size(), offset),
          "control corruption read failed");
  block[200] ^= 0x01U;
  Require(PwriteAll(fd, block.data(), block.size(), offset),
          "control corruption write failed");
  Require(fdatasync(fd) == 0, "control corruption fdatasync failed");
  close(fd);
}

using ControlMutator = void (*)(eufs::journal::JournalControl*);

void RewriteBothControls(const std::string& path,
                         const eufs::ondisk::Superblock& superblock,
                         ControlMutator mutate) {
  const int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
  Require(fd >= 0, "control rewrite open failed");
  eufs::ondisk::Block bytes{};
  Require(PreadAll(fd, bytes.data(), bytes.size(),
                   ControlOffset(superblock,
                                 eufs::journal::ControlCopy::kA)),
          "control rewrite read failed");
  eufs::journal::JournalControl control;
  std::string error;
  Require(eufs::journal::DecodeControl(bytes, &control, &error), error.c_str());
  mutate(&control);
  Require(eufs::journal::EncodeControl(control, &bytes, nullptr, &error),
          error.c_str());
  Require(PwriteAll(fd, bytes.data(), bytes.size(),
                    ControlOffset(superblock,
                                  eufs::journal::ControlCopy::kA)) &&
              PwriteAll(fd, bytes.data(), bytes.size(),
                        ControlOffset(superblock,
                                      eufs::journal::ControlCopy::kB)),
          "control rewrite write failed");
  Require(fdatasync(fd) == 0, "control rewrite fdatasync failed");
  close(fd);
}

void ChangeUuid(eufs::journal::JournalControl* control) {
  control->filesystem_uuid[0] ^= 0x80U;
}

void ChangeRingGeometry(eufs::journal::JournalControl* control) {
  ++control->ring_blocks;
}

void RequireOpenSelects(const std::string& path,
                        eufs::journal::ControlCopy expected_copy) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == 0,
          detail.c_str());
  Require(reader->journal_control_copy() == expected_copy,
          "reader selected the wrong journal control copy");
}

void RequireOpenRejects(const std::string& path) {
  std::unique_ptr<eufs::storage::ImageReader> reader;
  std::string detail;
  Require(eufs::storage::ImageReader::Open(path, &reader, &detail) == -EUCLEAN &&
              reader == nullptr,
          "reader accepted an image without a valid bound journal control");
}

}  // namespace

int main() {
  eufs::ondisk::Superblock superblock;

  std::string path = CreateImage(&superblock);
  RequireOpenSelects(path, eufs::journal::ControlCopy::kA);
  unlink(path.c_str());

  path = CreateImage(&superblock);
  CorruptControl(path, superblock, eufs::journal::ControlCopy::kA);
  RequireOpenSelects(path, eufs::journal::ControlCopy::kB);
  unlink(path.c_str());

  path = CreateImage(&superblock);
  CorruptControl(path, superblock, eufs::journal::ControlCopy::kB);
  RequireOpenSelects(path, eufs::journal::ControlCopy::kA);
  unlink(path.c_str());

  path = CreateImage(&superblock);
  CorruptControl(path, superblock, eufs::journal::ControlCopy::kA);
  CorruptControl(path, superblock, eufs::journal::ControlCopy::kB);
  RequireOpenRejects(path);
  unlink(path.c_str());

  path = CreateImage(&superblock);
  RewriteBothControls(path, superblock, ChangeUuid);
  RequireOpenRejects(path);
  unlink(path.c_str());

  path = CreateImage(&superblock);
  RewriteBothControls(path, superblock, ChangeRingGeometry);
  RequireOpenRejects(path);
  unlink(path.c_str());

  std::cout << "control_initial=A corrupt_A_select=B corrupt_B_select=A "
               "reject=both_uuid_geometry\n";
  std::cout << "PASS: journal control real-image integration test\n";
  return 0;
}
