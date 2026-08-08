#include "object/object_backend.h"

#include "journal/journal_transaction_executor.h"
#include "metadata/new_object_plan.h"
#include "metadata/object_replace_plan.h"

#include <cerrno>
#include <limits>
#include <map>
#include <sys/stat.h>
#include <utility>

namespace eufs::object_store {
namespace {

// 写入可选错误详情，允许调用者只关心负 errno。
void SetDetail(std::string* detail, std::string_view message) {
  if (detail != nullptr) {
    detail->assign(message);
  }
}

// 从完整 inode 中筛出对象接口承诺暴露的稳定字段。
ObjectStat MakeObjectStat(std::uint32_t inode_number,
                          const ondisk::InodeRecord& inode) {
  return ObjectStat{inode_number, inode.size, inode.mtime_ns,
                    inode.generation};
}

}  // 匿名命名空间：Backend 内部转换辅助函数不对外暴露。

// 私有构造函数只接收已经完成恢复和 reader 校验的完整资源集合。
ObjectBackend::ObjectBackend(
    ObjectBackendOptions options,
    std::unique_ptr<storage::MountedImageSession> session,
    std::unique_ptr<storage::ImageReader> reader,
    std::unique_ptr<RequestLedgerIndex> request_ledger,
    std::shared_ptr<journal::JournalControlIo> mutation_io,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer)
    : options_(options),
      session_(std::move(session)),
      reader_(std::move(reader)),
      request_ledger_(std::move(request_ledger)),
      mutation_io_(std::move(mutation_io)),
      mutation_observer_(std::move(mutation_observer)) {}

// 建立一个可被普通 C++/未来 RPC 直接调用的对象存储实例。
int ObjectBackend::Open(
    const std::string& image_path, const ObjectBackendOptions& options,
    std::unique_ptr<ObjectBackend>* output,
    journal::RecoveryAction* recovery_action, std::string* detail,
    std::shared_ptr<journal::JournalControlIo> recovery_io,
    std::shared_ptr<journal::JournalControlIo> mutation_io,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer) {
  // 检查必需输出地址和默认权限，权限只能包含低 9 位 POSIX rwx。
  if (image_path.empty() || output == nullptr || recovery_action == nullptr ||
      (options.permissions & ~0777U) != 0) {
    SetDetail(detail,
              "image path, valid options, backend output, and recovery action "
              "are required");
    return -EINVAL;
  }
  // 失败契约：开始前清空 output，后续任何失败都不能留下半初始化 Backend。
  output->reset();
  if (detail != nullptr) {
    detail->clear();
  }

  // session 在整个 Backend 生命周期持有镜像独占锁。
  std::unique_ptr<storage::MountedImageSession> session;
  int result = storage::MountedImageSession::Open(image_path, &session, detail);
  if (result != 0) {
    return result;
  }

  // 复制 fd 给恢复器；恢复结束前绝不创建 reader。
  int store_fd = -1;
  result = session->DuplicateFd(&store_fd, detail);
  if (result != 0) {
    return result;
  }
  std::unique_ptr<journal::JournalControlStore> store;
  result = journal::JournalControlStore::AdoptLockedFd(
      store_fd, &store, detail, std::move(recovery_io));
  if (result != 0) {
    return result;
  }
  // 根据持久化 control 严格丢弃未提交事务或回放已提交事务。
  journal::RecoveryAction action{};
  result = store->ResolveRecovery(&action, detail);
  if (result != 0) {
    return result;
  }
  // 恢复 store 完成使命后立即销毁，在线写入会为每个事务重新接管复制 fd。
  store.reset();

  // 只在恢复建立稳定 home metadata 边界后创建 reader。
  int reader_fd = -1;
  result = session->DuplicateFd(&reader_fd, detail);
  if (result != 0) {
    return result;
  }
  std::unique_ptr<storage::ImageReader> reader;
  result = storage::ImageReader::AdoptLockedFd(reader_fd, &reader, detail);
  if (result != 0) {
    return result;
  }

  // feature 声明的是强制启动契约：路径、记录前缀或索引任一损坏都拒绝发布 Backend。
  std::unique_ptr<RequestLedgerIndex> request_ledger;
  if ((reader->superblock().feature_incompat &
       ondisk::kFeatureIncompatRequestLedger) != 0) {
    auto candidate = std::make_unique<RequestLedgerIndex>();
    result = ScanRequestLedger(*reader, candidate.get(), detail);
    if (result != 0) {
      return result;
    }
    request_ledger = std::move(candidate);
  }

  // 所有阶段成功后才发布 Backend，并把实际恢复动作返回上层记录。
  output->reset(new ObjectBackend(options, std::move(session),
                                  std::move(reader), std::move(request_ledger),
                                  std::move(mutation_io),
                                  std::move(mutation_observer)));
  *recovery_action = action;
  return 0;
}

// 要求调用者已经持有 mutex_，统一检查 fail-closed 状态和核心资源。
int ObjectBackend::CheckUsableLocked(std::string* detail) const {
  if (fatal_error_ == 0 && session_ != nullptr && reader_ != nullptr) {
    return 0;
  }
  SetDetail(detail, fatal_detail_.empty() ? "object backend is unavailable"
                                          : fatal_detail_);
  return -EIO;
}

// 持久化状态不确定或 reader 重建失败后，永久关闭当前 Backend 的服务能力。
void ObjectBackend::FailClosedLocked(int error, std::string_view detail) {
  // 销毁 reader 防止后续误读旧快照。
  reader_.reset();
  fatal_error_ = error < 0 ? error : -EIO;
  fatal_detail_.assign(detail);
}

// 已提交 metadata 改变后重新解析镜像；candidate 保证失败时不覆盖旧指针。
int ObjectBackend::ReloadReaderLocked(std::string* detail) {
  int reader_fd = -1;
  int result = session_->DuplicateFd(&reader_fd, detail);
  if (result != 0) {
    FailClosedLocked(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  std::unique_ptr<storage::ImageReader> candidate;
  result = storage::ImageReader::AdoptLockedFd(reader_fd, &candidate, detail);
  if (result != 0) {
    FailClosedLocked(result, detail == nullptr ? std::string_view{} : *detail);
    return result;
  }
  // 新 reader 完整构造成功后才替换旧 reader。
  reader_ = std::move(candidate);
  return 0;
}

// 把无斜杠对象名映射为根路径 `/name`，并要求最终目标是普通文件。
int ObjectBackend::ResolveRegularLocked(std::string_view name,
                                        std::uint32_t* inode_number,
                                        ondisk::InodeRecord* inode,
                                        std::string* detail) const {
  if (!metadata::IsValidRootObjectName(name) || inode_number == nullptr ||
      inode == nullptr) {
    SetDetail(detail, "valid root object name and lookup outputs are required");
    return -EINVAL;
  }
  // Backend 对象命名空间当前固定为根目录单层名称。
  std::string path("/");
  path.append(name);
  const int result = reader_->ResolvePath(path, inode_number, inode, detail);
  if (result != 0) {
    return result;
  }
  if (!S_ISREG(inode->mode)) {
    SetDetail(detail, "object name does not resolve to a regular file");
    return -EISDIR;
  }
  return 0;
}

// 在已持有 Backend mutex 的前提下提交一个 planner 事务并重建 reader。
int ObjectBackend::ApplyLocked(
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>&
        ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    MutationOutcome* outcome, std::string* detail) {
  if (outcome == nullptr) {
    SetDetail(detail, "mutation outcome output is required");
    return -EINVAL;
  }
  // 执行器尚未成功前，默认结论是事务没有被应用。
  *outcome = MutationOutcome::kNotApplied;
  // 执行器明确告诉上层：失败是否已经进入持久化不确定区间。
  bool failure_requires_fail_closed = false;
  const int result = journal::ExecuteJournalTransaction(
      *session_, before_images, ordered_data_after_images,
      metadata_after_images, total_blocks, filesystem_uuid,
      mutation_io_, mutation_observer_, &failure_requires_fail_closed, detail);
  // 不确定失败立即销毁 reader；普通前置错误则只返回本次请求失败。
  if (result != 0 && failure_requires_fail_closed) {
    *outcome = MutationOutcome::kUnknown;
    FailClosedLocked(result, detail == nullptr ? std::string_view{} : *detail);
  }
  if (result != 0) {
    return result;
  }
  // executor 返回成功意味着 COMMIT、home replay 和 checkpoint 已全部完成。
  *outcome = MutationOutcome::kCommitted;
  // 成功提交后必须让后续请求观察新 inode/bitmap，而不是旧缓存。
  return ReloadReaderLocked(detail);
}

// 原子创建此前不存在的完整对象；同名对象存在时 planner 返回 EEXIST。
int ObjectBackend::PutIfAbsent(std::string_view name, std::string_view data,
                               std::uint64_t timestamp_ns,
                               std::string* detail) {
  MutationResult ignored;
  return PutIfAbsent(name, data, timestamp_ns, &ignored, detail);
}

// 与条件替换一样，把 errno 和持久化结果分开返回，供远程调用决定能否安全重试。
int ObjectBackend::PutIfAbsent(std::string_view name, std::string_view data,
                               std::uint64_t timestamp_ns,
                               MutationResult* output,
                               std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "creation result output is required");
    return -EINVAL;
  }
  MutationResult candidate;
  // 锁从检查旧命名空间一直持有到 COMMIT、checkpoint 和 reader 重建完成。
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    *output = candidate;
    return result;
  }

  // NewObjectPlan 同时规划 data、inode、dentry、bitmap 和可能的间接块。
  metadata::NewObjectPlan plan;
  result = metadata::PrepareNewRootObject(
      *reader_, name, data, options_.permissions, options_.uid, options_.gid,
      timestamp_ns, &plan, detail);
  if (result != 0) {
    if (result == -EEXIST) {
      std::uint32_t inode_number = 0;
      ondisk::InodeRecord inode;
      std::string ignored_detail;
      if (ResolveRegularLocked(name, &inode_number, &inode,
                               &ignored_detail) == 0) {
        candidate.current_version =
            ObjectVersion{inode_number, inode.generation};
      }
    }
    *output = candidate;
    return result;
  }
  result = ApplyLocked(plan.before_images, plan.ordered_data_after_images,
                       plan.metadata_after_images, plan.total_blocks,
                       plan.filesystem_uuid, &candidate.outcome, detail);
  if (candidate.outcome == MutationOutcome::kCommitted) {
    candidate.committed_version = ObjectVersion{plan.inode_number, 1};
    candidate.current_version = candidate.committed_version;
  } else if (candidate.outcome == MutationOutcome::kUnknown) {
    candidate.current_version = ObjectVersion{};
  }
  *output = candidate;
  return result;
}

// 对完整对象执行带版本条件的全量替换，避免两个并发写者互相静默覆盖。
int ObjectBackend::ReplaceIfVersion(std::string_view name,
                                    ObjectVersion expected,
                                    std::string_view data,
                                    std::uint64_t timestamp_ns,
                                    MutationResult* output,
                                    std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "replacement result output is required");
    return -EINVAL;
  }
  MutationResult candidate;

  // 同一把锁覆盖查找、版本检查、规划、提交和 reader 重建，形成一次线性化操作。
  const std::unique_lock<std::shared_mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    *output = candidate;
    return result;
  }

  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  result = ResolveRegularLocked(name, &inode_number, &inode, detail);
  if (result != 0) {
    *output = candidate;
    return result;
  }
  candidate.current_version = ObjectVersion{inode_number, inode.generation};
  // inode_number 也属于令牌：只比 generation 会把另一个 inode 生命周期误当同一对象。
  if (expected.inode_number != inode_number ||
      expected.generation != inode.generation) {
    SetDetail(detail, "object version no longer matches current object");
    *output = candidate;
    return -ESTALE;
  }

  metadata::ObjectReplacePlan plan;
  result = metadata::PrepareObjectReplace(
      *reader_, inode_number, expected.generation, data, timestamp_ns, &plan,
      detail);
  if (result != 0) {
    *output = candidate;
    return result;
  }

  result = ApplyLocked(plan.before_images, plan.ordered_data_after_images,
                       plan.metadata_after_images, plan.total_blocks,
                       plan.filesystem_uuid, &candidate.outcome, detail);
  if (candidate.outcome == MutationOutcome::kCommitted) {
    candidate.committed_version =
        ObjectVersion{inode_number, plan.new_generation};
    candidate.current_version = candidate.committed_version;
  } else if (candidate.outcome == MutationOutcome::kUnknown) {
    // 持久化结果未知后，提交前读到的旧令牌不能再冒充当前权威版本。
    candidate.current_version = ObjectVersion{};
  }
  *output = candidate;
  return result;
}

// 读取完整对象；不像 POSIX read，它没有 offset/size 分段接口。
int ObjectBackend::Get(std::string_view name, std::string* output,
                       std::string* detail) {
  ObjectStat ignored_stat;
  return Get(name, output, &ignored_stat, detail);
}

int ObjectBackend::Get(std::string_view name, std::string* output,
                       ObjectStat* stat, std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr || stat == nullptr) {
    SetDetail(detail, "object data and stat outputs are required");
    return -EINVAL;
  }
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    return result;
  }

  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  result = ResolveRegularLocked(name, &inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  if (inode.size > std::numeric_limits<std::size_t>::max()) {
    SetDetail(detail, "object size cannot be represented in memory");
    return -EOVERFLOW;
  }

  // 先在 candidate 中读取，任何中途错误都不修改调用者现有 output。
  std::string candidate(static_cast<std::size_t>(inode.size), '\0');
  std::size_t bytes_read = 0;
  if (!candidate.empty()) {
    result = reader_->ReadFile(
        inode_number, 0, reinterpret_cast<std::uint8_t*>(candidate.data()),
        candidate.size(), &bytes_read, detail);
    if (result != 0) {
      return result;
    }
  }
  // inode.size 与 reader 实际返回字节数矛盾说明镜像或 reader 契约损坏。
  if (bytes_read != candidate.size()) {
    SetDetail(detail, "object reader returned an incomplete payload");
    return -EUCLEAN;
  }
  // 完整成功后一次性移动给调用者。
  *output = std::move(candidate);
  *stat = MakeObjectStat(inode_number, inode);
  return 0;
}

// 只解析 inode 并返回筛选后的对象元数据。
int ObjectBackend::Stat(std::string_view name, ObjectStat* output,
                        std::string* detail) {
  if (detail != nullptr) {
    detail->clear();
  }
  if (output == nullptr) {
    SetDetail(detail, "object stat output is required");
    return -EINVAL;
  }
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  int result = CheckUsableLocked(detail);
  if (result != 0) {
    return result;
  }

  std::uint32_t inode_number = 0;
  ondisk::InodeRecord inode;
  result = ResolveRegularLocked(name, &inode_number, &inode, detail);
  if (result != 0) {
    return result;
  }
  *output = MakeObjectStat(inode_number, inode);
  return 0;
}

// 线程安全查询当前 Backend 是否仍可服务。
bool ObjectBackend::usable() const {
  const std::shared_lock<std::shared_mutex> lock(mutex_);
  return fatal_error_ == 0 && session_ != nullptr && reader_ != nullptr;
}

}  // namespace eufs::object_store
