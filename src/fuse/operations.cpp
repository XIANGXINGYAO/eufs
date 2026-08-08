#define FUSE_USE_VERSION 31

#include "fuse/operations.h"

#include "journal/journal_transaction_executor.h"
#include "metadata/empty_file_create_plan.h"
#include "metadata/file_write_plan.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <time.h>
#include <vector>

namespace eufs::fuse_adapter {
namespace {

// FUSE 回调签名由 libfuse 固定，不能额外传入 C++ 状态参数。
// eufsd_main 把 FuseMountState 作为 private_data 交给 fuse_main；
// 每个回调都通过该函数取回同一个挂载状态对象。
FuseMountState& State() {
  // private_data 原本以 void* 保存，这里恢复为真实类型并返回引用。
  return *static_cast<FuseMountState*>(fuse_get_context()->private_data);
}

// 把磁盘格式使用的纳秒时间戳拆成 POSIX timespec 的秒和纳秒两部分。
void SetTimestamp(std::uint64_t nanoseconds, timespec* output) {
  output->tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
  output->tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);
}

// 把 EUFS inode 的稳定字段翻译为内核 getattr 需要的 struct stat。
void FillStat(std::uint32_t inode_number,
              const ondisk::InodeRecord& inode, struct stat* output) {
  // 先清零全部未显式支持字段，避免把栈垃圾返回给内核。
  std::memset(output, 0, sizeof(*output));
  output->st_ino = inode_number;
  output->st_mode = inode.mode;
  output->st_uid = inode.uid;
  output->st_gid = inode.gid;
  output->st_nlink = inode.link_count;
  output->st_size = static_cast<off_t>(inode.size);
  output->st_blksize = ondisk::kBlockSize;
  // st_blocks 的单位固定为 512 字节，不是 EUFS 的 4096 字节块。
  output->st_blocks = static_cast<blkcnt_t>((inode.size + 511U) / 512U);
  SetTimestamp(inode.atime_ns, &output->st_atim);
  SetTimestamp(inode.mtime_ns, &output->st_mtim);
  SetTimestamp(inode.ctime_ns, &output->st_ctim);
}

// 统一执行路径解析，并在挂载 fail-closed 后拒绝继续读取。
int Resolve(const char* path, std::uint32_t* inode_number,
            ondisk::InodeRecord* inode) {
  if (path == nullptr) {
    return -EINVAL;
  }
  std::string detail;
  if (!State().usable()) {
    return -EIO;
  }
  return State().reader->ResolvePath(path, inode_number, inode, &detail);
}

// getattr 对应 stat/lstat：解析路径并返回类型、权限、大小和时间等元数据。
int Getattr(const char* path, struct stat* attributes,
            struct fuse_file_info*) {
  // reader 与写事务共享状态，因此读操作也必须在同一把挂载锁内执行。
  const std::lock_guard<std::mutex> lock(State().mutex);
  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  const int result = Resolve(path, &inode_number, &inode);
  if (result != 0) {
    return result;
  }
  FillStat(inode_number, inode, attributes);
  return 0;
}

// opendir 验证路径确实是目录，并把 inode 号保存为后续 readdir 的句柄。
int Opendir(const char* path, struct fuse_file_info* file_info) {
  const std::lock_guard<std::mutex> lock(State().mutex);
  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  const int result = Resolve(path, &inode_number, &inode);
  if (result != 0) {
    return result;
  }
  if (!S_ISDIR(inode.mode)) {
    return -ENOTDIR;
  }
  // FUSE fh 是用户态自定义的 64 位字段，这里直接保存稳定 inode 号。
  file_info->fh = inode_number;
  return 0;
}

// readdir 按 offset 继续枚举目录，支持内核缓冲区装满后再次进入回调。
int Readdir(const char* path, void* buffer, fuse_fill_dir_t filler,
            off_t offset, struct fuse_file_info* file_info,
            enum fuse_readdir_flags) {
  const std::lock_guard<std::mutex> lock(State().mutex);
  if (offset < 0) {
    return -EINVAL;
  }
  if (!State().usable()) {
    return -EIO;
  }
  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  // 有 opendir 句柄时直接读 inode，避免再次做路径解析；否则兼容无句柄调用。
  if (file_info != nullptr && file_info->fh != 0) {
    inode_number = static_cast<std::uint32_t>(file_info->fh);
    std::string detail;
    const int result =
        State().reader->ReadInode(inode_number, &inode, &detail);
    if (result != 0) {
      return result;
    }
  } else {
    const int result = Resolve(path, &inode_number, &inode);
    if (result != 0) {
      return result;
    }
  }
  if (!S_ISDIR(inode.mode)) {
    return -ENOTDIR;
  }

  // 从目录数据块解码真实目录项；`.` 和 `..` 由适配层补充。
  std::vector<ondisk::DirectoryEntry> entries;
  std::string detail;
  const int result =
      State().reader->ListDirectory(inode_number, &entries, &detail);
  if (result != 0) {
    return result;
  }

  // offset 是下次开始位置，前两个逻辑位置固定留给 `.` 和 `..`。
  constexpr auto kNoFillFlags = static_cast<fuse_fill_dir_flags>(0);
  const std::size_t start = static_cast<std::size_t>(offset);
  const std::size_t total = entries.size() + 2U;
  for (std::size_t index = start; index < total; ++index) {
    const char* name = nullptr;
    if (index == 0) {
      name = ".";
    } else if (index == 1) {
      name = "..";
    } else {
      name = entries[index - 2U].name.c_str();
    }
    // filler 返回非零表示内核提供的目录缓冲区已满，本轮正常停止而不是报错。
    if (filler(buffer, name, nullptr, static_cast<off_t>(index + 1U),
               kNoFillFlags) != 0) {
      break;
    }
  }
  return 0;
}

// 当前目录句柄没有额外堆资源，releasedir 无需清理。
int Releasedir(const char*, struct fuse_file_info*) { return 0; }

// open 校验访问模式和文件类型，再把 inode 号保存为后续 read/write 句柄。
int Open(const char* path, struct fuse_file_info* file_info) {
  const std::lock_guard<std::mutex> lock(State().mutex);
  const int access_mode = file_info->flags & O_ACCMODE;
  // v1 只支持只读或只写打开；O_RDWR 和 O_TRUNC 语义尚未实现。
  if ((access_mode != O_RDONLY && access_mode != O_WRONLY) ||
      (file_info->flags & O_TRUNC) != 0) {
    std::cerr << "eufsd: open rejected for unsupported access/truncate: "
              << (path == nullptr ? "<null>" : path) << "\n";
    return -EOPNOTSUPP;
  }
  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  const int result = Resolve(path, &inode_number, &inode);
  if (result != 0) {
    return result;
  }
  if (!S_ISREG(inode.mode)) {
    return -EISDIR;
  }
  file_info->fh = inode_number;
  return 0;
}

// read 从已打开句柄或路径取得 inode 号，再由 ImageReader 按 offset/size 读数据。
int Read(const char* path, char* buffer, size_t size, off_t offset,
         struct fuse_file_info* file_info) {
  const std::lock_guard<std::mutex> lock(State().mutex);
  if (offset < 0) {
    return -EINVAL;
  }
  if (!State().usable()) {
    return -EIO;
  }
  std::uint32_t inode_number = 0;
  if (file_info != nullptr && file_info->fh != 0) {
    inode_number = static_cast<std::uint32_t>(file_info->fh);
  } else {
    ondisk::InodeRecord inode;
    const int result = Resolve(path, &inode_number, &inode);
    if (result != 0) {
      return result;
    }
  }
  // 返回类型是 int，因此单次读取大小必须限制在 INT_MAX。
  const std::size_t limited_size =
      std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  std::size_t bytes_read = 0;
  std::string detail;
  // ImageReader 负责 direct/single-indirect 映射和 EOF 截断。
  const int result = State().reader->ReadFile(
      inode_number, static_cast<std::uint64_t>(offset),
      reinterpret_cast<std::uint8_t*>(buffer), limited_size, &bytes_read,
      &detail);
  if (result != 0) {
    return result;
  }
  return static_cast<int>(bytes_read);
}

// 当前文件句柄只保存 inode 号，没有额外资源需要释放。
int Release(const char*, struct fuse_file_info*) { return 0; }

// FUSE create/write 共用的唯一事务提交适配器。
int ApplyJournalTransaction(
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>& ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid, std::string* detail) {
  // 调用者必须在整个持久化协议期间持有挂载 mutex。
  // 当前事务发布元数据时，第二个回调不能基于旧 bitmap 生成新计划。
  bool failure_requires_fail_closed = false;
  int result = journal::ExecuteJournalTransaction(
      *State().session, before_images, ordered_data_after_images,
      metadata_after_images, total_blocks, filesystem_uuid,
      nullptr, State().mutation_observer, &failure_requires_fail_closed,
      detail);
  // 执行器确认进入不确定持久化区间后失败，整个挂载必须 fail-closed。
  if (result != 0 && failure_requires_fail_closed) {
    std::cerr << "eufsd: journal mutation failed: "
              << (detail == nullptr ? std::string_view{} : *detail)
              << " (errno " << -result << ")\n";
    State().FailClosed(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  // 在尚未进入事务持久化区间时失败，可以原样返回而不关闭挂载。
  if (result != 0) {
    return result;
  }
  // home metadata 已改变，旧 reader 缓存必须重建后才能服务下一请求。
  result = State().ReloadReader(detail);
  if (result != 0) {
    std::cerr << "eufsd: Reader reload after journal mutation failed: "
              << (detail == nullptr ? std::string_view{} : *detail)
              << " (errno " << -result << ")\n";
  }
  return result;
}

// create 当前只支持在根目录创建空普通文件。
int Create(const char* path, mode_t mode, struct fuse_file_info* file_info) {
  // FUSE 传入的关键指针必须有效。
  if (path == nullptr || file_info == nullptr) {
    return -EINVAL;
  }
  // v1 尚未支持嵌套目录创建，因此只接受 `/name` 形式。
  const std::string_view full_path(path);
  if (full_path.size() < 2 || full_path.front() != '/' ||
      full_path.find('/', 1) != std::string_view::npos) {
    return -EOPNOTSUPP;
  }

  // 从查重、计划生成到事务完成始终持锁，防止并发请求复用同一空闲位。
  const std::lock_guard<std::mutex> lock(State().mutex);
  if (!State().usable()) {
    return -EIO;
  }
  // 先解析同名路径：存在返回 EEXIST，其他读取错误不能伪装成“不存在”。
  std::uint32_t existing_inode = 0;
  ondisk::InodeRecord existing;
  std::string detail;
  const int lookup_result = State().reader->ResolvePath(
      full_path, &existing_inode, &existing, &detail);
  if (lookup_result == 0) {
    return -EEXIST;
  }
  if (lookup_result != -ENOENT) {
    return lookup_result;
  }

  // inode 时间戳来自真实 CLOCK_REALTIME，并转换为无符号纳秒。
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
    return -EIO;
  }
  const std::uint64_t timestamp_ns =
      static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
      static_cast<std::uint64_t>(now.tv_nsec);
  // FUSE context 提供发起系统调用进程的 uid/gid，而不是守护进程自身身份。
  const auto* context = fuse_get_context();

  // planner 只在内存生成 inode、目录项和 bitmap 的 before/after images。
  metadata::EmptyFileCreatePlan plan;
  int result = metadata::PrepareRootEmptyFileCreate(
      *State().reader, full_path.substr(1), static_cast<std::uint32_t>(mode),
      static_cast<std::uint32_t>(context->uid),
      static_cast<std::uint32_t>(context->gid), timestamp_ns, &plan, &detail);
  if (result != 0) {
    std::cerr << "eufsd: create plan failed: " << detail << " (errno "
              << -result << ")\n";
    return result;
  }

  // 创建空文件没有 ordered data；只有 metadata after-image 进入日志。
  const std::map<std::uint32_t, ondisk::Block> no_ordered_data;
  result = ApplyJournalTransaction(
      plan.before_images, no_ordered_data, plan.after_images,
      plan.total_blocks, plan.filesystem_uuid, &detail);
  if (result != 0) {
    return result;
  }
  // 成功后把新 inode 号作为刚创建文件的 FUSE 句柄返回。
  file_info->fh = plan.inode_number;
  return 0;
}

// write 实现一次 FUSE 回调范围内的 COW 写入和原子 metadata 提交。
int Write(const char* path, const char* buffer, size_t size, off_t offset,
          struct fuse_file_info* file_info) {
  if (buffer == nullptr || file_info == nullptr || offset < 0) {
    return -EINVAL;
  }
  // 零字节写入按 POSIX 语义直接成功，不生成空事务。
  if (size == 0) {
    return 0;
  }
  // FUSE 回调返回 int，拒绝无法表示的超大单次请求。
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return -EFBIG;
  }

  // 锁覆盖读取旧映射、生成 COW 计划、提交和 reader 重建全过程。
  const std::lock_guard<std::mutex> lock(State().mutex);
  if (!State().usable()) {
    return -EIO;
  }

  // 优先使用 open 保存的 inode 句柄；没有句柄时才重新解析路径。
  std::uint32_t inode_number = 0;
  if (file_info->fh != 0) {
    inode_number = static_cast<std::uint32_t>(file_info->fh);
  } else {
    ondisk::InodeRecord inode;
    const int resolve_result = Resolve(path, &inode_number, &inode);
    if (resolve_result != 0) {
      return resolve_result;
    }
  }

  // mtime/ctime 使用本次写请求开始提交时的真实时间。
  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
    return -EIO;
  }
  const std::uint64_t timestamp_ns =
      static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
      static_cast<std::uint64_t>(now.tv_nsec);

  std::string detail;
  if (State().mutation_observer != nullptr) {
    std::cerr << "eufsd: write transaction inode=" << inode_number
              << " offset=" << offset << " size=" << size << '\n';
  }
  // planner 根据旧 inode 映射生成新 COW 数据块和 metadata after-images。
  metadata::FileWritePlan plan;
  int result = metadata::PrepareFileWrite(
      *State().reader, inode_number, static_cast<std::uint64_t>(offset),
      std::string_view(buffer, size), timestamp_ns, &plan, &detail);
  if (result != 0) {
    std::cerr << "eufsd: write plan failed: " << detail << " (errno "
              << -result << ")\n";
    return result;
  }

  // 按唯一 WAL 执行器提交；失败契约由 ApplyJournalTransaction 统一处理。
  result = ApplyJournalTransaction(
      plan.before_images, plan.ordered_data_after_images,
      plan.metadata_after_images, plan.total_blocks, plan.filesystem_uuid,
      &detail);
  if (result != 0) {
    return result;
  }
  return static_cast<int>(size);
}
// 以下操作尚未实现，明确返回只读错误，避免 libfuse 猜测默认行为。
int ReadOnlyTruncate(const char*, off_t, struct fuse_file_info*) {
  return -EROFS;
}
int ReadOnlyMkdir(const char*, mode_t) { return -EROFS; }
int ReadOnlyUnlink(const char*) { return -EROFS; }
int ReadOnlyRmdir(const char*) { return -EROFS; }
int ReadOnlyRename(const char*, const char*, unsigned int) { return -EROFS; }

// FUSE 初始化回调：关闭内核缓存，确保每次观察都进入用户态当前 reader。
void* Init(struct fuse_conn_info*, struct fuse_config* config) {
  config->kernel_cache = 0;
  config->nullpath_ok = 0;
  config->use_ino = 1;
  return fuse_get_context()->private_data;
}

}  // 匿名命名空间：具体回调只通过 MakeOperations 函数表暴露。

// 构造并返回 libfuse 使用的回调函数指针表。
fuse_operations MakeOperations() {
  // 零初始化保证未实现回调保持 nullptr，而不是未定义地址。
  fuse_operations operations{};
  operations.init = Init;
  operations.getattr = Getattr;
  operations.opendir = Opendir;
  operations.readdir = Readdir;
  operations.releasedir = Releasedir;
  operations.open = Open;
  operations.read = Read;
  operations.release = Release;
  operations.create = Create;
  operations.write = Write;
  operations.truncate = ReadOnlyTruncate;
  operations.mkdir = ReadOnlyMkdir;
  operations.unlink = ReadOnlyUnlink;
  operations.rmdir = ReadOnlyRmdir;
  operations.rename = ReadOnlyRename;
  return operations;
}

}  // namespace eufs::fuse_adapter
