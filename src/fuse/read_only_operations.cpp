#define FUSE_USE_VERSION 31

#include "fuse/read_only_operations.h"

#include "metadata/empty_file_create_plan.h"
#include "metadata/file_write_plan.h"
#include "storage/writable_image.h"

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

StageCState& State() {
  return *static_cast<StageCState*>(fuse_get_context()->private_data);
}

void SetTimestamp(std::uint64_t nanoseconds, timespec* output) {
  output->tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
  output->tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);
}

void FillStat(std::uint32_t inode_number,
              const ondisk::InodeRecord& inode, struct stat* output) {
  std::memset(output, 0, sizeof(*output));
  output->st_ino = inode_number;
  output->st_mode = inode.mode;
  output->st_uid = inode.uid;
  output->st_gid = inode.gid;
  output->st_nlink = inode.link_count;
  output->st_size = static_cast<off_t>(inode.size);
  output->st_blksize = ondisk::kBlockSize;
  output->st_blocks = static_cast<blkcnt_t>((inode.size + 511U) / 512U);
  SetTimestamp(inode.atime_ns, &output->st_atim);
  SetTimestamp(inode.mtime_ns, &output->st_mtim);
  SetTimestamp(inode.ctime_ns, &output->st_ctim);
}

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

int Getattr(const char* path, struct stat* attributes,
            struct fuse_file_info*) {
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
  file_info->fh = inode_number;
  return 0;
}

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

  std::vector<ondisk::DirectoryEntry> entries;
  std::string detail;
  const int result =
      State().reader->ListDirectory(inode_number, &entries, &detail);
  if (result != 0) {
    return result;
  }

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
    if (filler(buffer, name, nullptr, static_cast<off_t>(index + 1U),
               kNoFillFlags) != 0) {
      break;
    }
  }
  return 0;
}

int Releasedir(const char*, struct fuse_file_info*) { return 0; }

int Open(const char* path, struct fuse_file_info* file_info) {
  const std::lock_guard<std::mutex> lock(State().mutex);
  const int access_mode = file_info->flags & O_ACCMODE;
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
  const std::size_t limited_size =
      std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()));
  std::size_t bytes_read = 0;
  std::string detail;
  const int result = State().reader->ReadFile(
      inode_number, static_cast<std::uint64_t>(offset),
      reinterpret_cast<std::uint8_t*>(buffer), limited_size, &bytes_read,
      &detail);
  if (result != 0) {
    return result;
  }
  return static_cast<int>(bytes_read);
}

int Release(const char*, struct fuse_file_info*) { return 0; }

int ApplyJournalTransaction(
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>& ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid, std::string* detail) {
  int mutation_fd = -1;
  int result = State().session->DuplicateFd(&mutation_fd, detail);
  if (result != 0) {
    return result;
  }

  std::unique_ptr<journal::JournalControlStore> store;
  result = journal::JournalControlStore::AdoptLockedFd(
      mutation_fd, &store, detail, nullptr, State().mutation_observer);
  if (result != 0) {
    std::cerr << "eufsd: journal mutation failed: "
              << (detail == nullptr ? std::string_view{} : *detail)
              << " (errno " << -result << ")\n";
    State().FailClosed(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }

  journal::DurableJournalBody body;
  result = store->WriteOrderedDataAndUnexposedBody(
      total_blocks, filesystem_uuid, before_images, ordered_data_after_images,
      metadata_after_images, &body, detail);
  if (result == 0) {
    result = store->ExposeDurableBody(detail);
  }
  if (result == 0) {
    result = store->WriteCommit(detail);
  }
  if (result == 0) {
    result = store->CompleteCommittedTransaction(detail);
  }
  if (result != 0) {
    std::cerr << "eufsd: journal mutation failed: "
              << (detail == nullptr ? std::string_view{} : *detail)
              << " (errno " << -result << ")\n";
    State().FailClosed(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  result = State().ReloadReader(detail);
  if (result != 0) {
    std::cerr << "eufsd: Reader reload after journal mutation failed: "
              << (detail == nullptr ? std::string_view{} : *detail)
              << " (errno " << -result << ")\n";
  }
  return result;
}

int Create(const char* path, mode_t mode, struct fuse_file_info* file_info) {
  if (path == nullptr || file_info == nullptr) {
    return -EINVAL;
  }
  const std::string_view full_path(path);
  if (full_path.size() < 2 || full_path.front() != '/' ||
      full_path.find('/', 1) != std::string_view::npos) {
    return -EOPNOTSUPP;
  }

  const std::lock_guard<std::mutex> lock(State().mutex);
  if (!State().usable()) {
    return -EIO;
  }
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

  timespec now{};
  if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0) {
    return -EIO;
  }
  const std::uint64_t timestamp_ns =
      static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
      static_cast<std::uint64_t>(now.tv_nsec);
  const auto* context = fuse_get_context();
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

  const std::map<std::uint32_t, ondisk::Block> no_ordered_data;
  result = ApplyJournalTransaction(
      plan.before_images, no_ordered_data, plan.after_images,
      plan.total_blocks, plan.filesystem_uuid, &detail);
  if (result != 0) {
    return result;
  }
  file_info->fh = plan.inode_number;
  return 0;
}

int Write(const char* path, const char* buffer, size_t size, off_t offset,
          struct fuse_file_info* file_info) {
  if (buffer == nullptr || file_info == nullptr || offset < 0) {
    return -EINVAL;
  }
  if (size == 0) {
    return 0;
  }
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return -EFBIG;
  }

  const std::lock_guard<std::mutex> lock(State().mutex);
  if (!State().usable()) {
    return -EIO;
  }

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
  metadata::FileWritePlan plan;
  int result = metadata::PrepareFileWrite(
      *State().reader, inode_number, static_cast<std::uint64_t>(offset),
      std::string_view(buffer, size), timestamp_ns, &plan, &detail);
  if (result != 0) {
    std::cerr << "eufsd: write plan failed: " << detail << " (errno "
              << -result << ")\n";
    return result;
  }

  result = ApplyJournalTransaction(
      plan.before_images, plan.ordered_data_after_images,
      plan.metadata_after_images, plan.total_blocks, plan.filesystem_uuid,
      &detail);
  if (result != 0) {
    return result;
  }
  return static_cast<int>(size);
}
int ReadOnlyTruncate(const char*, off_t, struct fuse_file_info*) {
  return -EROFS;
}
int ReadOnlyMkdir(const char*, mode_t) { return -EROFS; }
int ReadOnlyUnlink(const char*) { return -EROFS; }
int ReadOnlyRmdir(const char*) { return -EROFS; }
int ReadOnlyRename(const char*, const char*, unsigned int) { return -EROFS; }

void* Init(struct fuse_conn_info*, struct fuse_config* config) {
  config->kernel_cache = 0;
  config->nullpath_ok = 0;
  config->use_ino = 1;
  return fuse_get_context()->private_data;
}

}  // namespace

fuse_operations MakeReadOnlyOperations() {
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
