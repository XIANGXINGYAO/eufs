#include "storage/writable_image.h"

#include "metadata/ondisk_format.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <set>
#include <string_view>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace eufs::storage {
namespace {

struct DirectMutationPlan {
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> after_images;
  std::vector<std::uint32_t> write_order;
};

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
  int Release() {
    const int value = value_;
    value_ = -1;
    return value;
  }

 private:
  int value_;
};

void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

int PreadAll(int fd, std::uint8_t* output, std::size_t size,
             std::uint64_t offset, std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, "pread image", error_number);
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

int PwriteAll(int fd, const std::uint8_t* input, std::size_t size,
              std::uint64_t offset, std::string* detail) {
  std::size_t completed = 0;
  while (completed < size) {
    const auto result = pwrite(fd, input + completed, size - completed,
                               static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      SetSystemDetail(detail, "pwrite image", error_number);
      return -error_number;
    }
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

template <typename Plan>
int ValidatePlan(const Plan& plan, std::string* detail) {
  if (plan.after_images.empty() ||
      plan.write_order.size() != plan.after_images.size()) {
    SetDetail(detail, "mutation plan is incomplete");
    return -EINVAL;
  }

  const std::set<std::uint32_t> ordered_blocks(plan.write_order.begin(),
                                                plan.write_order.end());
  if (ordered_blocks.size() != plan.write_order.size()) {
    SetDetail(detail, "mutation plan write order contains duplicate blocks");
    return -EINVAL;
  }
  for (const std::uint32_t block : plan.write_order) {
    if (plan.after_images.find(block) == plan.after_images.end() ||
        block >= plan.total_blocks) {
      SetDetail(detail,
                "mutation plan write order references an invalid block");
      return -EINVAL;
    }
  }
  return 0;
}

template <typename Plan>
int ApplyPlanOnLockedFd(int locked_fd, const Plan& plan,
                        std::string* detail) {
  FileDescriptor fd(locked_fd);
  if (locked_fd < 0) {
    SetDetail(detail, "valid locked mutation fd is required");
    return -EINVAL;
  }
  if (detail != nullptr) {
    detail->clear();
  }
  int result = ValidatePlan(plan, detail);
  if (result != 0) {
    return result;
  }

  const int status_flags = fcntl(fd.get(), F_GETFL);
  if (status_flags < 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "inspect adopted mutation fd", error_number);
    return -error_number;
  }
  if ((status_flags & O_ACCMODE) != O_RDWR) {
    SetDetail(detail, "adopted mutation fd is not open for read and write");
    return -EACCES;
  }

  struct stat image_stat {};
  if (fstat(fd.get(), &image_stat) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "fstat image", error_number);
    return -error_number;
  }
  if (image_stat.st_size !=
      static_cast<off_t>(static_cast<std::uint64_t>(plan.total_blocks) *
                         ondisk::kBlockSize)) {
    SetDetail(detail, "image size changed after mutation planning");
    return -ESTALE;
  }

  ondisk::Block superblock_bytes{};
  result = PreadAll(fd.get(), superblock_bytes.data(), superblock_bytes.size(),
                    0, detail);
  if (result != 0) {
    return result;
  }
  ondisk::Superblock superblock;
  if (!ondisk::DecodeSuperblock(superblock_bytes, &superblock, detail) ||
      superblock.filesystem_uuid != plan.filesystem_uuid ||
      superblock.total_blocks != plan.total_blocks) {
    SetDetail(detail, "image identity changed after mutation planning");
    return -ESTALE;
  }

  for (const auto& [block_number, expected] : plan.before_images) {
    if (block_number >= plan.total_blocks) {
      SetDetail(detail,
                "mutation plan before-image block is outside the image");
      return -EINVAL;
    }
    ondisk::Block current{};
    result = PreadAll(
        fd.get(), current.data(), current.size(),
        static_cast<std::uint64_t>(block_number) * ondisk::kBlockSize, detail);
    if (result != 0) {
      return result;
    }
    if (current != expected) {
      SetDetail(detail, "home block changed after mutation planning");
      return -ESTALE;
    }
  }

  for (const std::uint32_t block_number : plan.write_order) {
    const auto after = plan.after_images.find(block_number);
    result = PwriteAll(
        fd.get(), after->second.data(), after->second.size(),
        static_cast<std::uint64_t>(block_number) * ondisk::kBlockSize, detail);
    if (result != 0) {
      return result;
    }
  }
  if (fdatasync(fd.get()) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "fdatasync image", error_number);
    return -error_number;
  }
  return 0;
}

template <typename Plan>
int ApplyPlan(const std::string& image_path, const Plan& plan,
              std::string* detail) {
  if (image_path.empty()) {
    SetDetail(detail, "image path is required");
    return -EINVAL;
  }
  if (detail != nullptr) {
    detail->clear();
  }
  const int validation_result = ValidatePlan(plan, detail);
  if (validation_result != 0) {
    return validation_result;
  }

  const int raw_fd = open(image_path.c_str(), O_RDWR | O_CLOEXEC);
  if (raw_fd < 0) {
    SetSystemDetail(detail, "open image", errno);
    return -errno;
  }
  FileDescriptor fd(raw_fd);
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "lock image", error_number);
    return error_number == EWOULDBLOCK ? -EBUSY : -error_number;
  }
  return ApplyPlanOnLockedFd(fd.Release(), plan, detail);
}

template <typename Plan>
int BuildJournalLikeDirectMutationPlan(const Plan& plan,
                                       DirectMutationPlan* output,
                                       std::string* detail) {
  if (output == nullptr) {
    SetDetail(detail, "direct mutation plan output is required");
    return -EINVAL;
  }
  *output = {};
  output->total_blocks = plan.total_blocks;
  output->filesystem_uuid = plan.filesystem_uuid;
  output->before_images = plan.before_images;

  for (const auto& [block, after_image] : plan.ordered_data_after_images) {
    output->after_images.emplace(block, after_image);
    output->write_order.push_back(block);
  }
  for (const auto& [block, after_image] : plan.metadata_after_images) {
    if (!output->after_images.emplace(block, after_image).second) {
      SetDetail(detail,
                "journal-like plan classifies a block as data and metadata");
      return -EINVAL;
    }
    output->write_order.push_back(block);
  }
  return 0;
}

}  // namespace

int ApplyCreatePlan(const std::string& image_path,
                    const metadata::EmptyFileCreatePlan& plan,
                    std::string* detail) {
  return ApplyPlan(image_path, plan, detail);
}

int ApplyCreatePlanOnLockedFd(int locked_fd,
                              const metadata::EmptyFileCreatePlan& plan,
                              std::string* detail) {
  return ApplyPlanOnLockedFd(locked_fd, plan, detail);
}

int ApplyFirstBlockWritePlan(const std::string& image_path,
                             const metadata::FirstBlockWritePlan& plan,
                             std::string* detail) {
  DirectMutationPlan direct;
  const int result =
      BuildJournalLikeDirectMutationPlan(plan, &direct, detail);
  if (result != 0) {
    return result;
  }
  return ApplyPlan(image_path, direct, detail);
}

int ApplyFirstBlockWritePlanOnLockedFd(
    int locked_fd, const metadata::FirstBlockWritePlan& plan,
    std::string* detail) {
  FileDescriptor fd(locked_fd);
  DirectMutationPlan direct;
  const int result =
      BuildJournalLikeDirectMutationPlan(plan, &direct, detail);
  if (result != 0) {
    return result;
  }
  return ApplyPlanOnLockedFd(fd.Release(), direct, detail);
}

int ApplyFileWritePlan(const std::string& image_path,
                       const metadata::FileWritePlan& plan,
                       std::string* detail) {
  DirectMutationPlan direct;
  const int result =
      BuildJournalLikeDirectMutationPlan(plan, &direct, detail);
  if (result != 0) {
    return result;
  }
  return ApplyPlan(image_path, direct, detail);
}

int ApplyFileWritePlanOnLockedFd(int locked_fd,
                                 const metadata::FileWritePlan& plan,
                                 std::string* detail) {
  FileDescriptor fd(locked_fd);
  DirectMutationPlan direct;
  const int result =
      BuildJournalLikeDirectMutationPlan(plan, &direct, detail);
  if (result != 0) {
    return result;
  }
  return ApplyPlanOnLockedFd(fd.Release(), direct, detail);
}

}  // namespace eufs::storage
