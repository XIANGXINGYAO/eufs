#pragma once

#include "journal/ondisk_journal.h"
#include "metadata/ondisk_format.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eufs::storage {

class ImageReader {
 public:
  ~ImageReader();

  ImageReader(const ImageReader&) = delete;
  ImageReader& operator=(const ImageReader&) = delete;

  static int Open(const std::string& path,
                  std::unique_ptr<ImageReader>* output,
                  std::string* detail);

  // Consumes locked_fd on every return path without changing its flock state.
  static int AdoptLockedFd(int locked_fd,
                           std::unique_ptr<ImageReader>* output,
                           std::string* detail);

  const ondisk::Superblock& superblock() const { return superblock_; }
  const journal::JournalControl& journal_control() const {
    return journal_control_;
  }
  journal::ControlCopy journal_control_copy() const {
    return journal_control_copy_;
  }

  bool IsInodeAllocated(std::uint32_t inode_number) const;
  bool IsBlockAllocated(std::uint32_t block_number) const;
  int ReadBlock(std::uint32_t block_number, ondisk::Block* output,
                std::string* detail) const;

  int ReadInode(std::uint32_t inode_number, ondisk::InodeRecord* output,
                std::string* detail) const;
  int MapLogicalBlock(const ondisk::InodeRecord& inode,
                      std::uint32_t logical_block,
                      std::uint32_t* physical_block,
                      std::string* detail) const;
  int ListDirectory(std::uint32_t inode_number,
                    std::vector<ondisk::DirectoryEntry>* output,
                    std::string* detail) const;
  int ResolvePath(std::string_view path, std::uint32_t* inode_number,
                  ondisk::InodeRecord* inode, std::string* detail) const;
  int ReadFile(std::uint32_t inode_number, std::uint64_t offset,
               std::uint8_t* output, std::size_t requested,
               std::size_t* bytes_read, std::string* detail) const;

 private:
  ImageReader(int fd, std::uint64_t image_size,
              const ondisk::Superblock& superblock,
              const journal::JournalControl& journal_control,
              journal::ControlCopy journal_control_copy,
              std::vector<std::uint8_t> inode_bitmap,
              std::vector<std::uint8_t> block_bitmap);

  int ReadExact(std::uint64_t offset, std::uint8_t* output, std::size_t size,
                std::string* detail) const;
  int ValidateAllocatedDataBlock(std::uint32_t block_number,
                                 std::string* detail) const;

  int fd_;
  std::uint64_t image_size_;
  ondisk::Superblock superblock_;
  journal::JournalControl journal_control_;
  journal::ControlCopy journal_control_copy_;
  std::vector<std::uint8_t> inode_bitmap_;
  std::vector<std::uint8_t> block_bitmap_;
};

}  // namespace eufs::storage
