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

// 镜像的只读解析视图。构造时缓存已验证 superblock、A/B control 和两个 bitmap；
// inode、目录及数据块按请求从 fd 定位读取。任何在线提交后都必须重建该对象。
class ImageReader {
 public:
  // 关闭本对象接管的镜像 fd。
  ~ImageReader();

  // fd 与缓存视图必须只有一个所有者，禁止复制。
  ImageReader(const ImageReader&) = delete;
  ImageReader& operator=(const ImageReader&) = delete;

  // 独立只读打开镜像并完成格式、几何、control 和 bitmap 基础校验。
  static int Open(const std::string& path,
                  std::unique_ptr<ImageReader>* output,
                  std::string* detail);

  // 接管已加锁 fd；无论成功失败都负责关闭，不改变该 open-file-description 的 flock。
  static int AdoptLockedFd(int locked_fd,
                           std::unique_ptr<ImageReader>* output,
                           std::string* detail);

  // 返回打开时缓存的稳定 superblock 和被选中的 journal control。
  const ondisk::Superblock& superblock() const { return superblock_; }
  const journal::JournalControl& journal_control() const {
    return journal_control_;
  }
  journal::ControlCopy journal_control_copy() const {
    return journal_control_copy_;
  }

  // 查询缓存 bitmap；只回答分配状态，不验证 inode/块内容。
  bool IsInodeAllocated(std::uint32_t inode_number) const;
  bool IsBlockAllocated(std::uint32_t block_number) const;
  // 完整读取一个物理块，并检查块号位于镜像范围内。
  int ReadBlock(std::uint32_t block_number, ondisk::Block* output,
                std::string* detail) const;

  // 从 inode table 定位、解码并验证一个已分配 inode。
  int ReadInode(std::uint32_t inode_number, ondisk::InodeRecord* output,
                std::string* detail) const;
  // 把文件逻辑块映射为 direct 或 single-indirect 物理数据块。
  int MapLogicalBlock(const ondisk::InodeRecord& inode,
                      std::uint32_t logical_block,
                      std::uint32_t* physical_block,
                      std::string* detail) const;
  // 解码目录 inode 的全部有效目录项，严格尊重 record_length。
  int ListDirectory(std::uint32_t inode_number,
                    std::vector<ondisk::DirectoryEntry>* output,
                    std::string* detail) const;
  // 从根 inode 逐级解析绝对路径，并返回目标 inode 号和记录。
  int ResolvePath(std::string_view path, std::uint32_t* inode_number,
                  ondisk::InodeRecord* inode, std::string* detail) const;
  // 按 offset/requested 读取普通文件，bytes_read 返回受 EOF 限制后的实际字节数。
  int ReadFile(std::uint32_t inode_number, std::uint64_t offset,
               std::uint8_t* output, std::size_t requested,
               std::size_t* bytes_read, std::string* detail) const;

 private:
  // 只有 Open/AdoptLockedFd 完成全部启动校验后才能构造 reader。
  ImageReader(int fd, std::uint64_t image_size,
              const ondisk::Superblock& superblock,
              const journal::JournalControl& journal_control,
              journal::ControlCopy journal_control_copy,
              std::vector<std::uint8_t> inode_bitmap,
              std::vector<std::uint8_t> block_bitmap);

  // 循环 pread 直到完整读取指定字节区间。
  int ReadExact(std::uint64_t offset, std::uint8_t* output, std::size_t size,
                std::string* detail) const;
  // 验证块位于 data 区且在缓存 block bitmap 中已分配。
  int ValidateAllocatedDataBlock(std::uint32_t block_number,
                                 std::string* detail) const;

  // 以下成员共同描述同一个打开时刻的稳定磁盘快照。
  int fd_;
  std::uint64_t image_size_;
  ondisk::Superblock superblock_;
  journal::JournalControl journal_control_;
  journal::ControlCopy journal_control_copy_;
  std::vector<std::uint8_t> inode_bitmap_;
  std::vector<std::uint8_t> block_bitmap_;
};

}  // namespace eufs::storage
