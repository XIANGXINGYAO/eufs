#include "storage/mkfs.h"

#include "journal/ondisk_journal.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string_view>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace eufs::storage {
namespace {

class FileDescriptor {
 public:
  explicit FileDescriptor(int value) : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      close(value_);
    }
  }

  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;

  int get() const { return value_; }

 private:
  int value_;
};

class PartialImageCleanup {
 public:
  PartialImageCleanup(const std::string& path, bool active)
      : path_(path), active_(active) {}
  ~PartialImageCleanup() {
    if (active_) {
      unlink(path_.c_str());
    }
  }

  PartialImageCleanup(const PartialImageCleanup&) = delete;
  PartialImageCleanup& operator=(const PartialImageCleanup&) = delete;

  void Release() { active_ = false; }

 private:
  const std::string& path_;
  bool active_;
};

void SetError(std::string* error, std::string_view message) {
  if (error != nullptr) {
    error->assign(message);
  }
}

void SetSystemError(std::string* error, std::string_view operation,
                    int error_number) {
  if (error != nullptr) {
    error->assign(operation);
    error->append(": ");
    error->append(std::strerror(error_number));
  }
}

bool PwriteAll(int fd, const std::uint8_t* data, std::size_t size,
               std::uint64_t offset, std::string* error) {
  std::size_t written = 0;
  while (written < size) {
    const auto result = pwrite(fd, data + written, size - written,
                               static_cast<off_t>(offset + written));
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetSystemError(error, "pwrite", errno);
      return false;
    }
    if (result == 0) {
      SetError(error, "pwrite made no progress");
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

void SetBitmapBit(std::vector<std::uint8_t>* bitmap, std::uint32_t bit) {
  (*bitmap)[bit / 8U] |= static_cast<std::uint8_t>(1U << (bit % 8U));
}

void SetBitmapPrefix(std::vector<std::uint8_t>* bitmap,
                     std::uint32_t used_bits) {
  const std::size_t full_bytes = used_bits / 8U;
  std::fill(bitmap->begin(), bitmap->begin() + full_bytes, 0xFFU);
  const std::uint32_t remaining_bits = used_bits % 8U;
  if (remaining_bits != 0) {
    (*bitmap)[full_bytes] =
        static_cast<std::uint8_t>((1U << remaining_bits) - 1U);
  }
}

void ReserveBitmapTail(std::vector<std::uint8_t>* bitmap,
                       std::uint32_t valid_bits) {
  const std::size_t first_tail_byte = valid_bits / 8U;
  const std::uint32_t valid_bits_in_byte = valid_bits % 8U;
  if (valid_bits_in_byte != 0) {
    const std::uint8_t tail_mask =
        static_cast<std::uint8_t>(0xFFU << valid_bits_in_byte);
    (*bitmap)[first_tail_byte] |= tail_mask;
    std::fill(bitmap->begin() + first_tail_byte + 1, bitmap->end(), 0xFFU);
  } else {
    std::fill(bitmap->begin() + first_tail_byte, bitmap->end(), 0xFFU);
  }
}

bool FillRandom(std::uint8_t* output, std::size_t size, std::string* error) {
  std::size_t filled = 0;
  while (filled < size) {
    const auto result = getrandom(output + filled, size - filled, 0);
    if (result < 0) {
      if (errno == EINTR) {
        continue;
      }
      SetSystemError(error, "getrandom", errno);
      return false;
    }
    filled += static_cast<std::size_t>(result);
  }
  return true;
}

bool CurrentTimeNs(std::uint64_t* output, std::string* error) {
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
    SetSystemError(error, "clock_gettime", errno);
    return false;
  }
  if (now.tv_sec < 0 ||
      static_cast<std::uint64_t>(now.tv_sec) >
          (std::numeric_limits<std::uint64_t>::max() -
           static_cast<std::uint64_t>(now.tv_nsec)) /
              1000000000ULL) {
    SetError(error, "current time does not fit the on-disk timestamp");
    return false;
  }
  *output = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
            static_cast<std::uint64_t>(now.tv_nsec);
  return true;
}

}  // namespace

bool FormatImage(const MkfsOptions& options,
                 ondisk::Superblock* formatted_superblock,
                 std::string* error) {
  if (options.image_path.empty() || options.image_size_bytes == 0 ||
      options.image_size_bytes % ondisk::kBlockSize != 0) {
    SetError(error, "image path and block-aligned image size are required");
    return false;
  }
  const std::uint64_t total_blocks64 =
      options.image_size_bytes / ondisk::kBlockSize;
  if (total_blocks64 > std::numeric_limits<std::uint32_t>::max() ||
      options.image_size_bytes >
          static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
    SetError(error, "image exceeds the v1 block-number or host offset limit");
    return false;
  }

  ondisk::Superblock superblock;
  if (!ondisk::BuildSuperblockLayout(
          static_cast<std::uint32_t>(total_blocks64), options.total_inodes,
          options.journal_blocks, &superblock, error)) {
    return false;
  }
  if (!FillRandom(superblock.filesystem_uuid.data(),
                  superblock.filesystem_uuid.size(), error) ||
      !CurrentTimeNs(&superblock.created_time_ns, error)) {
    return false;
  }
  superblock.filesystem_uuid[6] = static_cast<std::uint8_t>(
      (superblock.filesystem_uuid[6] & 0x0FU) | 0x40U);
  superblock.filesystem_uuid[8] = static_cast<std::uint8_t>(
      (superblock.filesystem_uuid[8] & 0x3FU) | 0x80U);

  const int flags =
      O_RDWR | O_CREAT | O_CLOEXEC | (options.force ? 0 : O_EXCL);
  const int raw_fd = open(options.image_path.c_str(), flags, 0644);
  if (raw_fd < 0) {
    SetSystemError(error, "open image", errno);
    return false;
  }
  FileDescriptor fd(raw_fd);
  PartialImageCleanup cleanup(options.image_path, !options.force);
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    SetSystemError(error, "lock image", errno);
    return false;
  }

  if (options.force) {
    ondisk::Block invalid_superblock{};
    if (!PwriteAll(fd.get(), invalid_superblock.data(),
                   invalid_superblock.size(), 0, error)) {
      return false;
    }
    if (fdatasync(fd.get()) != 0) {
      SetSystemError(error, "fdatasync invalidated superblock", errno);
      return false;
    }
  }

  if (ftruncate(fd.get(), static_cast<off_t>(options.image_size_bytes)) != 0) {
    SetSystemError(error, "ftruncate image", errno);
    return false;
  }
  const int allocation_error =
      posix_fallocate(fd.get(), 0, static_cast<off_t>(options.image_size_bytes));
  if (allocation_error != 0) {
    SetSystemError(error, "posix_fallocate image", allocation_error);
    return false;
  }

  std::vector<std::uint8_t> inode_bitmap(
      static_cast<std::size_t>(superblock.inode_bitmap.block_count) *
          ondisk::kBlockSize,
      0);
  SetBitmapBit(&inode_bitmap, superblock.root_inode - 1U);
  ReserveBitmapTail(&inode_bitmap, superblock.total_inodes);

  std::vector<std::uint8_t> block_bitmap(
      static_cast<std::size_t>(superblock.block_bitmap.block_count) *
          ondisk::kBlockSize,
      0);
  SetBitmapPrefix(&block_bitmap, superblock.data.start_block);
  ReserveBitmapTail(&block_bitmap, superblock.total_blocks);

  ondisk::InodeRecord root;
  root.inode_number = superblock.root_inode;
  root.mode = S_IFDIR | 0755;
  root.uid = static_cast<std::uint32_t>(getuid());
  root.gid = static_cast<std::uint32_t>(getgid());
  root.link_count = 2;
  root.atime_ns = superblock.created_time_ns;
  root.mtime_ns = superblock.created_time_ns;
  root.ctime_ns = superblock.created_time_ns;
  root.generation = 1;
  ondisk::InodeBytes root_bytes{};
  if (!ondisk::EncodeInode(root, &root_bytes, error)) {
    return false;
  }

  journal::JournalControl clean_control;
  clean_control.ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  clean_control.filesystem_uuid = superblock.filesystem_uuid;
  clean_control.generation = 0;
  clean_control.head = 0;
  clean_control.tail = 0;
  clean_control.used_blocks = 0;
  clean_control.next_transaction_id = 1;
  ondisk::Block control_bytes{};
  if (!journal::EncodeControl(clean_control, &control_bytes, nullptr, error)) {
    return false;
  }

  if (!PwriteAll(fd.get(), inode_bitmap.data(), inode_bitmap.size(),
                 static_cast<std::uint64_t>(
                     superblock.inode_bitmap.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), block_bitmap.data(), block_bitmap.size(),
                 static_cast<std::uint64_t>(
                     superblock.block_bitmap.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), root_bytes.data(), root_bytes.size(),
                 static_cast<std::uint64_t>(
                     superblock.inode_table.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), control_bytes.data(), control_bytes.size(),
                 static_cast<std::uint64_t>(superblock.journal.start_block) *
                     ondisk::kBlockSize,
                 error) ||
      !PwriteAll(fd.get(), control_bytes.data(), control_bytes.size(),
                 static_cast<std::uint64_t>(
                     superblock.journal.start_block + 1U) *
                     ondisk::kBlockSize,
                 error)) {
    return false;
  }
  if (fdatasync(fd.get()) != 0) {
    SetSystemError(error, "fdatasync filesystem body", errno);
    return false;
  }

  ondisk::Block superblock_bytes{};
  if (!ondisk::EncodeSuperblock(superblock, &superblock_bytes, error) ||
      !PwriteAll(fd.get(), superblock_bytes.data(), superblock_bytes.size(), 0,
                 error)) {
    return false;
  }
  if (fdatasync(fd.get()) != 0) {
    SetSystemError(error, "fdatasync superblock", errno);
    return false;
  }

  if (formatted_superblock != nullptr) {
    *formatted_superblock = superblock;
  }
  cleanup.Release();
  return true;
}

}  // namespace eufs::storage
