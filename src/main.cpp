#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <linux/fs.h>
#include <map>
#include <mutex>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {

using InodeId = std::uint64_t;

constexpr InodeId kRootInode = 1;
constexpr InodeId kHelloInode = 2;
constexpr InodeId kNoteInode = 3;
constexpr InodeId kFirstDynamicInode = 4;

struct InMemoryInode {
  mode_t mode;
  nlink_t link_count;
  std::uint64_t open_count;
  std::string content;
  std::map<std::string, InodeId> entries;
};

struct FileSystemState {
  std::mutex mutex;
  InodeId next_inode{kFirstDynamicInode};
  std::unordered_map<InodeId, InMemoryInode> inodes{
      {kRootInode,
       {S_IFDIR | 0755,
        2,
        0,
        {},
        {{"hello.txt", kHelloInode}, {"note.txt", kNoteInode}}}},
      {kHelloInode, {S_IFREG | 0444, 1, 0, "hello from eufs\n", {}}},
      {kNoteInode, {S_IFREG | 0444, 1, 0, "eufs note\n", {}}},
  };
};

FileSystemState& filesystem_state() {
  return *static_cast<FileSystemState*>(fuse_get_context()->private_data);
}

int lookup_inode_id(const FileSystemState& state, std::string_view path,
                    InodeId* inode_id) {
  if (path.empty() || path.front() != '/') {
    return -EINVAL;
  }
  if (path == "/") {
    *inode_id = kRootInode;
    return 0;
  }

  InodeId current = kRootInode;
  size_t component_start = 1;
  while (component_start < path.size()) {
    const size_t slash = path.find('/', component_start);
    const size_t component_end =
        slash == std::string_view::npos ? path.size() : slash;
    if (component_end == component_start) {
      return -EINVAL;
    }

    const std::string_view component =
        path.substr(component_start, component_end - component_start);
    if (component == "." || component == "..") {
      return -EINVAL;
    }

    const auto directory = state.inodes.find(current);
    if (directory == state.inodes.end()) {
      return -EIO;
    }
    if (!S_ISDIR(directory->second.mode)) {
      return -ENOTDIR;
    }

    const auto entry = directory->second.entries.find(std::string(component));
    if (entry == directory->second.entries.end()) {
      return -ENOENT;
    }
    current = entry->second;

    if (slash == std::string_view::npos) {
      break;
    }
    component_start = slash + 1;
    if (component_start == path.size()) {
      return -EINVAL;
    }
  }

  if (state.inodes.find(current) == state.inodes.end()) {
    return -EIO;
  }
  *inode_id = current;
  return 0;
}

InodeId find_inode_id(const FileSystemState& state, std::string_view path) {
  InodeId inode_id = 0;
  return lookup_inode_id(state, path, &inode_id) == 0 ? inode_id : 0;
}

const InMemoryInode* find_inode(const FileSystemState& state,
                                std::string_view path) {
  const InodeId inode_id = find_inode_id(state, path);
  if (inode_id == 0) {
    return nullptr;
  }

  const auto inode = state.inodes.find(inode_id);
  return inode == state.inodes.end() ? nullptr : &inode->second;
}

const InMemoryInode* resolve_inode(const FileSystemState& state,
                                   const char* path,
                                   const struct fuse_file_info* file_info) {
  if (file_info != nullptr && file_info->fh != 0) {
    const auto inode = state.inodes.find(file_info->fh);
    return inode == state.inodes.end() ? nullptr : &inode->second;
  }
  return path == nullptr ? nullptr : find_inode(state, path);
}

InMemoryInode* resolve_inode(FileSystemState& state, const char* path,
                             const struct fuse_file_info* file_info) {
  return const_cast<InMemoryInode*>(resolve_inode(
      static_cast<const FileSystemState&>(state), path, file_info));
}

int resize_inode(InMemoryInode& inode, off_t new_size) {
  if (new_size < 0) {
    return -EINVAL;
  }
  if (static_cast<std::uintmax_t>(new_size) >
      std::numeric_limits<size_t>::max()) {
    return -EFBIG;
  }

  const size_t length = static_cast<size_t>(new_size);
  if (length > inode.content.max_size()) {
    return -EFBIG;
  }

  try {
    inode.content.resize(length, '\0');
  } catch (const std::length_error&) {
    return -EFBIG;
  } catch (const std::bad_alloc&) {
    return -ENOSPC;
  }
  return 0;
}

struct ParentLookup {
  InodeId inode_id;
  std::string name;
};

int lookup_parent(const FileSystemState& state, std::string_view path,
                  ParentLookup* parent) {
  if (path.size() < 2 || path.front() != '/' || path.back() == '/') {
    return path == "/" ? -EBUSY : -EINVAL;
  }

  const size_t slash = path.rfind('/');
  const std::string_view name = path.substr(slash + 1);
  if (name.empty() || name == "." || name == "..") {
    return -EINVAL;
  }

  const std::string_view parent_path =
      slash == 0 ? std::string_view("/") : path.substr(0, slash);
  InodeId parent_inode = 0;
  const int result = lookup_inode_id(state, parent_path, &parent_inode);
  if (result != 0) {
    return result;
  }

  const auto inode = state.inodes.find(parent_inode);
  if (inode == state.inodes.end()) {
    return -EIO;
  }
  if (!S_ISDIR(inode->second.mode)) {
    return -ENOTDIR;
  }

  parent->inode_id = parent_inode;
  parent->name.assign(name);
  return 0;
}

int eufs_getattr(const char* path, struct stat* st,
                 struct fuse_file_info* file_info) {
  std::memset(st, 0, sizeof(*st));

  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto* inode = resolve_inode(state, path, file_info);
  if (inode == nullptr) {
    return -ENOENT;
  }

  st->st_mode = inode->mode;
  st->st_nlink = inode->link_count;
  if (S_ISREG(inode->mode)) {
    st->st_size = static_cast<off_t>(inode->content.size());
  }
  return 0;
}

int eufs_opendir(const char* path, struct fuse_file_info* file_info) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  InodeId inode_id = 0;
  const int result = lookup_inode_id(state, path, &inode_id);
  if (result != 0) {
    return result;
  }

  const auto inode = state.inodes.find(inode_id);
  if (inode == state.inodes.end()) {
    return -EIO;
  }
  if (!S_ISDIR(inode->second.mode)) {
    return -ENOTDIR;
  }
  if (inode->second.open_count ==
      std::numeric_limits<std::uint64_t>::max()) {
    return -EMFILE;
  }

  ++inode->second.open_count;
  file_info->fh = inode_id;
  return 0;
}

int eufs_readdir(const char* path, void* buffer, fuse_fill_dir_t filler,
                 off_t, struct fuse_file_info* file_info,
                 enum fuse_readdir_flags) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto* directory = resolve_inode(state, path, file_info);
  if (directory == nullptr) {
    return -ENOENT;
  }
  if (!S_ISDIR(directory->mode)) {
    return -ENOTDIR;
  }

  constexpr auto kNoFillFlags = static_cast<fuse_fill_dir_flags>(0);
  filler(buffer, ".", nullptr, 0, kNoFillFlags);
  filler(buffer, "..", nullptr, 0, kNoFillFlags);
  for (const auto& [name, inode] : directory->entries) {
    static_cast<void>(inode);
    filler(buffer, name.c_str(), nullptr, 0, kNoFillFlags);
  }
  return 0;
}

int eufs_open(const char* path, struct fuse_file_info* file_info) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const InodeId inode_id = find_inode_id(state, path);
  const auto inode_entry = state.inodes.find(inode_id);
  if (inode_id == 0 || inode_entry == state.inodes.end()) {
    return -ENOENT;
  }
  auto* inode = &inode_entry->second;
  if (!S_ISREG(inode->mode)) {
    return -EISDIR;
  }

  const int access_mode = file_info->flags & O_ACCMODE;
  if (access_mode != O_RDONLY && access_mode != O_WRONLY &&
      access_mode != O_RDWR) {
    return -EINVAL;
  }
  if (access_mode != O_RDONLY && (inode->mode & 0222) == 0) {
    return -EACCES;
  }
  if ((file_info->flags & O_TRUNC) != 0) {
    if (access_mode == O_RDONLY) {
      return -EACCES;
    }
    const int result = resize_inode(*inode, 0);
    if (result != 0) {
      return result;
    }
  }
  if (inode->open_count == std::numeric_limits<std::uint64_t>::max()) {
    return -EMFILE;
  }
  ++inode->open_count;
  file_info->fh = inode_id;
  return 0;
}

int eufs_truncate(const char* path, off_t new_size,
                  struct fuse_file_info* file_info) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  auto* inode = resolve_inode(state, path, file_info);
  if (inode == nullptr) {
    return -ENOENT;
  }
  if (!S_ISREG(inode->mode)) {
    return -EISDIR;
  }
  if ((inode->mode & 0222) == 0) {
    return -EACCES;
  }
  return resize_inode(*inode, new_size);
}

int eufs_read(const char* path, char* buffer, size_t size, off_t offset,
              struct fuse_file_info* file_info) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto* inode = resolve_inode(state, path, file_info);
  if (inode == nullptr) {
    return -ENOENT;
  }
  if (!S_ISREG(inode->mode)) {
    return -EISDIR;
  }
  if (offset < 0 || static_cast<size_t>(offset) >= inode->content.size()) {
    return 0;
  }

  const size_t available = inode->content.size() - static_cast<size_t>(offset);
  const size_t bytes = std::min(size, available);
  std::memcpy(buffer, inode->content.data() + offset, bytes);
  return static_cast<int>(bytes);
}

int eufs_write(const char* path, const char* buffer, size_t size, off_t offset,
               struct fuse_file_info* file_info) {
  if (file_info == nullptr) {
    return -EBADF;
  }
  const bool append = (file_info->flags & O_APPEND) != 0;
  if (!append && offset < 0) {
    return -EINVAL;
  }
  if (size > static_cast<size_t>(std::numeric_limits<int>::max()) ||
      (!append && static_cast<std::uintmax_t>(offset) >
                      std::numeric_limits<size_t>::max())) {
    return -EFBIG;
  }

  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  auto* inode = resolve_inode(state, path, file_info);
  if (inode == nullptr) {
    return -ENOENT;
  }
  if (!S_ISREG(inode->mode)) {
    return -EISDIR;
  }
  if ((file_info->flags & O_ACCMODE) == O_RDONLY) {
    return -EBADF;
  }
  if (size == 0) {
    return 0;
  }

  const size_t start = append ? inode->content.size()
                              : static_cast<size_t>(offset);
  if (size > std::numeric_limits<size_t>::max() - start) {
    return -EFBIG;
  }
  const size_t end = start + size;
  if (end > inode->content.max_size()) {
    return -EFBIG;
  }

  try {
    if (end > inode->content.size()) {
      inode->content.resize(end, '\0');
    }
  } catch (const std::length_error&) {
    return -EFBIG;
  } catch (const std::bad_alloc&) {
    return -ENOSPC;
  }

  std::memcpy(inode->content.data() + start, buffer, size);
  return static_cast<int>(size);
}

int eufs_create(const char* path, mode_t mode,
                struct fuse_file_info* file_info) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  ParentLookup parent{};
  const int lookup_result = lookup_parent(state, path, &parent);
  if (lookup_result != 0) {
    return lookup_result;
  }

  const auto parent_inode = state.inodes.find(parent.inode_id);
  if (parent_inode == state.inodes.end()) {
    return -EIO;
  }
  if (parent_inode->second.entries.find(parent.name) !=
      parent_inode->second.entries.end()) {
    return -EEXIST;
  }

  const InodeId inode_id = state.next_inode;
  const auto [inode, inode_inserted] = state.inodes.emplace(
      inode_id, InMemoryInode{S_IFREG | (mode & 0777), 1, 1, {}, {}});
  if (!inode_inserted) {
    return -EIO;
  }

  const auto current_parent = state.inodes.find(parent.inode_id);
  if (current_parent == state.inodes.end()) {
    state.inodes.erase(inode);
    return -EIO;
  }
  try {
    const auto [entry, entry_inserted] =
        current_parent->second.entries.emplace(parent.name, inode_id);
    static_cast<void>(entry);
    if (!entry_inserted) {
      state.inodes.erase(inode);
      return -EEXIST;
    }
  } catch (const std::bad_alloc&) {
    state.inodes.erase(inode);
    return -ENOSPC;
  }

  ++state.next_inode;
  file_info->fh = inode_id;
  return 0;
}

int eufs_mkdir(const char* path, mode_t mode) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  ParentLookup parent{};
  const int lookup_result = lookup_parent(state, path, &parent);
  if (lookup_result != 0) {
    return lookup_result;
  }

  const auto parent_inode = state.inodes.find(parent.inode_id);
  if (parent_inode == state.inodes.end()) {
    return -EIO;
  }
  if (parent_inode->second.entries.find(parent.name) !=
      parent_inode->second.entries.end()) {
    return -EEXIST;
  }
  if (parent_inode->second.link_count ==
      std::numeric_limits<nlink_t>::max()) {
    return -EMLINK;
  }

  const InodeId inode_id = state.next_inode;
  const auto [inode, inode_inserted] = state.inodes.emplace(
      inode_id, InMemoryInode{S_IFDIR | (mode & 0777), 2, 0, {}, {}});
  if (!inode_inserted) {
    return -EIO;
  }

  const auto current_parent = state.inodes.find(parent.inode_id);
  if (current_parent == state.inodes.end()) {
    state.inodes.erase(inode);
    return -EIO;
  }
  try {
    const auto [entry, entry_inserted] =
        current_parent->second.entries.emplace(parent.name, inode_id);
    static_cast<void>(entry);
    if (!entry_inserted) {
      state.inodes.erase(inode);
      return -EEXIST;
    }
  } catch (const std::bad_alloc&) {
    state.inodes.erase(inode);
    return -ENOSPC;
  }

  ++current_parent->second.link_count;
  ++state.next_inode;
  return 0;
}

int eufs_unlink(const char* path) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  ParentLookup parent{};
  const int lookup_result = lookup_parent(state, path, &parent);
  if (lookup_result != 0) {
    return lookup_result;
  }

  const auto parent_inode = state.inodes.find(parent.inode_id);
  if (parent_inode == state.inodes.end()) {
    return -EIO;
  }
  const auto entry = parent_inode->second.entries.find(parent.name);
  if (entry == parent_inode->second.entries.end()) {
    return -ENOENT;
  }

  const InodeId inode_id = entry->second;
  const auto inode = state.inodes.find(inode_id);
  if (inode == state.inodes.end()) {
    return -EIO;
  }
  if (!S_ISREG(inode->second.mode)) {
    return -EISDIR;
  }
  if (inode->second.link_count == 0) {
    return -EIO;
  }

  parent_inode->second.entries.erase(entry);
  --inode->second.link_count;
  if (inode->second.link_count == 0 && inode->second.open_count == 0) {
    state.inodes.erase(inode);
  }
  return 0;
}

int eufs_rmdir(const char* path) {
  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  ParentLookup parent{};
  const int lookup_result = lookup_parent(state, path, &parent);
  if (lookup_result != 0) {
    return lookup_result;
  }

  const auto parent_inode = state.inodes.find(parent.inode_id);
  if (parent_inode == state.inodes.end()) {
    return -EIO;
  }
  const auto entry = parent_inode->second.entries.find(parent.name);
  if (entry == parent_inode->second.entries.end()) {
    return -ENOENT;
  }

  const auto inode = state.inodes.find(entry->second);
  if (inode == state.inodes.end()) {
    return -EIO;
  }
  if (!S_ISDIR(inode->second.mode)) {
    return -ENOTDIR;
  }
  if (!inode->second.entries.empty()) {
    return -ENOTEMPTY;
  }
  if (parent_inode->second.link_count == 0) {
    return -EIO;
  }

  parent_inode->second.entries.erase(entry);
  --parent_inode->second.link_count;
  inode->second.link_count = 0;
  if (inode->second.open_count == 0) {
    state.inodes.erase(inode);
  }
  return 0;
}

int eufs_rename(const char* old_path, const char* new_path,
                unsigned int flags) {
  if ((flags & RENAME_EXCHANGE) != 0) {
    return -EOPNOTSUPP;
  }
  if ((flags & ~static_cast<unsigned int>(RENAME_NOREPLACE)) != 0) {
    return -EINVAL;
  }

  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  ParentLookup old_parent{};
  ParentLookup new_parent{};
  const int old_result = lookup_parent(state, old_path, &old_parent);
  if (old_result != 0) {
    return old_result;
  }
  const int new_result = lookup_parent(state, new_path, &new_parent);
  if (new_result != 0) {
    return new_result;
  }

  const auto old_parent_inode = state.inodes.find(old_parent.inode_id);
  const auto new_parent_inode = state.inodes.find(new_parent.inode_id);
  if (old_parent_inode == state.inodes.end() ||
      new_parent_inode == state.inodes.end()) {
    return -EIO;
  }

  auto& old_entries = old_parent_inode->second.entries;
  auto& new_entries = new_parent_inode->second.entries;
  const auto source = old_entries.find(old_parent.name);
  if (source == old_entries.end()) {
    return -ENOENT;
  }
  const auto source_inode = state.inodes.find(source->second);
  if (source_inode == state.inodes.end()) {
    return -EIO;
  }
  if (!S_ISREG(source_inode->second.mode)) {
    return -EISDIR;
  }

  const auto target = new_entries.find(new_parent.name);
  if ((flags & RENAME_NOREPLACE) != 0 &&
      target != new_entries.end()) {
    return -EEXIST;
  }
  if ((old_parent.inode_id == new_parent.inode_id &&
       old_parent.name == new_parent.name) ||
      (target != new_entries.end() &&
       target->second == source->second)) {
    return 0;
  }

  if (target == new_entries.end()) {
    try {
      const auto [inserted_entry, inserted] =
          new_entries.emplace(new_parent.name, source->second);
      static_cast<void>(inserted_entry);
      if (!inserted) {
        return -EIO;
      }
    } catch (const std::bad_alloc&) {
      return -ENOSPC;
    }
    old_entries.erase(source);
    return 0;
  }

  const auto replaced_inode = state.inodes.find(target->second);
  if (replaced_inode == state.inodes.end() ||
      replaced_inode->second.link_count == 0) {
    return -EIO;
  }
  if (!S_ISREG(replaced_inode->second.mode)) {
    return -EISDIR;
  }

  target->second = source->second;
  old_entries.erase(source);
  --replaced_inode->second.link_count;
  if (replaced_inode->second.link_count == 0 &&
      replaced_inode->second.open_count == 0) {
    state.inodes.erase(replaced_inode);
  }
  return 0;
}

int eufs_release(const char*, struct fuse_file_info* file_info) {
  if (file_info == nullptr || file_info->fh == 0) {
    return -EBADF;
  }

  auto& state = filesystem_state();
  const std::lock_guard<std::mutex> lock(state.mutex);
  const auto inode = state.inodes.find(file_info->fh);
  if (inode == state.inodes.end() || inode->second.open_count == 0) {
    return -EBADF;
  }

  --inode->second.open_count;
  if (inode->second.link_count == 0 && inode->second.open_count == 0) {
    state.inodes.erase(inode);
  }
  return 0;
}

void* eufs_init(struct fuse_conn_info*, struct fuse_config* config) {
  config->hard_remove = 1;
  config->nullpath_ok = 1;
  return fuse_get_context()->private_data;
}

}  // namespace

int main(int argc, char* argv[]) {
  FileSystemState state;
  fuse_operations operations{};
  operations.init = eufs_init;
  operations.getattr = eufs_getattr;
  operations.opendir = eufs_opendir;
  operations.readdir = eufs_readdir;
  operations.releasedir = eufs_release;
  operations.open = eufs_open;
  operations.truncate = eufs_truncate;
  operations.read = eufs_read;
  operations.write = eufs_write;
  operations.create = eufs_create;
  operations.mkdir = eufs_mkdir;
  operations.unlink = eufs_unlink;
  operations.rmdir = eufs_rmdir;
  operations.rename = eufs_rename;
  operations.release = eufs_release;
  return fuse_main(argc, argv, &operations, &state);
}
