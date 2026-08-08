#include "tests/support/writable_image.h"

// 本文件是测试 fixture 写入器：绕过在线 WAL，把计划结果直接写进临时镜像。
// 这样可以精确构造恢复前状态；生产 eufsd 绝不能调用这里的直接覆盖逻辑。
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

// 把生产 planner 的两类 after image 合并为“按指定顺序直接覆盖”的测试计划。
// total_blocks 和 UUID 用来确认应用计划时面对的仍是生成计划时那一份镜像。
struct DirectMutationPlan {
  std::uint32_t total_blocks{0};
  std::array<std::uint8_t, 16> filesystem_uuid{};
  std::map<std::uint32_t, ondisk::Block> before_images;
  std::map<std::uint32_t, ondisk::Block> after_images;
  std::vector<std::uint32_t> write_order;
};

// 测试辅助器同样使用 RAII，确保所有提前返回路径都会关闭接管的 fd。
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

// detail 是可选输出；为空时调用者只关心负 errno。
void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

// 给系统调用错误补上操作名称和 strerror，便于定位 fixture 构造失败的位置。
void SetSystemDetail(std::string* detail, std::string_view operation,
                     int error_number) {
  if (detail != nullptr) {
    detail->assign(operation);
    detail->append(": ");
    detail->append(std::strerror(error_number));
  }
}

// pread 可能短读或被信号打断，因此循环到指定区间全部读完。
int PreadAll(int fd, std::uint8_t* output, std::size_t size,
             std::uint64_t offset, std::string* detail) {
  std::size_t completed = 0;
  // completed 始终表示已经可靠放入 output 的字节数。
  while (completed < size) {
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    if (result < 0 && errno == EINTR) {
      // EINTR 没有消费数据，从同一偏移重试。
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

// 与 PreadAll 对称：保证指定区间全部写入内核后才返回成功。
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

// 在接触镜像前检查计划自身闭合：每个 after image 必须恰好出现在写序列一次。
template <typename Plan>
int ValidatePlan(const Plan& plan, std::string* detail) {
  if (plan.after_images.empty() ||
      plan.write_order.size() != plan.after_images.size()) {
    SetDetail(detail, "mutation plan is incomplete");
    return -EINVAL;
  }

  // set 去重后的数量不同，说明同一物理块会被按同一计划重复覆盖。
  const std::set<std::uint32_t> ordered_blocks(plan.write_order.begin(),
                                                plan.write_order.end());
  if (ordered_blocks.size() != plan.write_order.size()) {
    SetDetail(detail, "mutation plan write order contains duplicate blocks");
    return -EINVAL;
  }
  // 写序列中的每个块都必须有对应 after image，而且不能越过镜像末尾。
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

// 接管已经持有锁的读写 fd，在 before-image 验证通过后直接应用测试计划。
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

  // 不能只相信函数名中的 LockedFd；至少验证它确实以 O_RDWR 打开。
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

  // planner 记录了 total_blocks；镜像长度变化说明计划已经过期。
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

  // 长度相同仍可能被替换，因此继续核对 superblock 中的 UUID 和块数。
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

  // before image 是乐观并发校验：任一 home block 变化都拒绝套用旧计划。
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

  // 校验全部完成后才开始覆盖；顺序由 fixture 场景明确指定。
  for (const std::uint32_t block_number : plan.write_order) {
    const auto after = plan.after_images.find(block_number);
    result = PwriteAll(
        fd.get(), after->second.data(), after->second.size(),
        static_cast<std::uint64_t>(block_number) * ondisk::kBlockSize, detail);
    if (result != 0) {
      return result;
    }
  }
  // 所有块写完后统一 fdatasync，保证后续恢复测试读取的是确定状态。
  if (fdatasync(fd.get()) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "fdatasync image", error_number);
    return -error_number;
  }
  return 0;
}

// 路径版入口只负责打开并取得非阻塞独占锁，真正应用逻辑复用上面的 fd 版本。
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
  // 另一个挂载会话持锁时返回 -EBUSY，测试辅助器不能绕过单写者约束。
  if (flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
    const int error_number = errno;
    SetSystemDetail(detail, "lock image", error_number);
    return error_number == EWOULDBLOCK ? -EBUSY : -error_number;
  }
  return ApplyPlanOnLockedFd(fd.Release(), plan, detail);
}

// 把 ordered data 放在 metadata 前面，模拟生产 WAL 在 COMMIT 前的数据先行顺序。
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

  // COW 数据先加入 after_images 和 write_order。
  for (const auto& [block, after_image] : plan.ordered_data_after_images) {
    output->after_images.emplace(block, after_image);
    output->write_order.push_back(block);
  }
  // 元数据随后加入；同一块被同时归类为数据和元数据属于 planner 错误。
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

}  // 匿名命名空间。

// 空文件创建计划本来就带有统一 after_images/write_order，可直接应用。
int ApplyCreatePlan(const std::string& image_path,
                    const metadata::EmptyFileCreatePlan& plan,
                    std::string* detail) {
  return ApplyPlan(image_path, plan, detail);
}

// 已锁 fd 版本用于 MountedImageSession 接管测试。
int ApplyCreatePlanOnLockedFd(int locked_fd,
                              const metadata::EmptyFileCreatePlan& plan,
                              std::string* detail) {
  return ApplyPlanOnLockedFd(locked_fd, plan, detail);
}

// 旧单块写计划先转换成统一直接写计划，再复用相同校验和落盘过程。
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

// 该重载接管调用者传入的 fd；FileDescriptor 保证转换失败时也会关闭它。
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

// 当前通用 FileWritePlan 的路径版 fixture 应用入口。
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

// 当前通用 FileWritePlan 的已锁 fd fixture 应用入口。
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
