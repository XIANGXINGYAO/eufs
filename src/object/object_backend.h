#pragma once

#include "journal/journal_control_store.h"
#include "storage/image_reader.h"
#include "storage/mounted_image_session.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace eufs::object_store {

// Backend 创建新对象时使用的固定 POSIX 元数据；未来 RPC 层不直接控制磁盘字段。
struct ObjectBackendOptions {
  std::uint32_t permissions{0644};
  std::uint32_t uid{0};
  std::uint32_t gid{0};
};

// Stat 接口返回的稳定对象元数据，不暴露完整内部 inode 结构。
struct ObjectStat {
  std::uint32_t inode_number{0};
  std::uint64_t size{0};
  std::uint64_t mtime_ns{0};
  std::uint64_t generation{0};
};

// 条件替换使用的稳定版本令牌；inode 槽位和 generation 必须同时匹配。
struct ObjectVersion {
  std::uint32_t inode_number{0};
  std::uint64_t generation{0};
};

// errno 描述错误类别，outcome 单独描述本次变更是否已经成为持久化事实。
enum class MutationOutcome {
  kNotApplied,
  kCommitted,
  kUnknown,
};

struct MutationResult {
  MutationOutcome outcome{MutationOutcome::kNotApplied};
  // 成功提交或已知提交时返回新令牌。
  ObjectVersion committed_version{};
  // ESTALE/确定未应用时可返回当前令牌；kUnknown 时必须为空。
  ObjectVersion current_version{};
};

// 不依赖 FUSE 的进程内对象存储核心。未来 brpc handler 直接调用该类；
// 它仍通过 session/image reader/journal 操作 eufs.img，但不经过挂载点和 FUSE 回调。
class ObjectBackend {
 public:
  // session、reader 和锁状态只能由一个 Backend 拥有，禁止复制。
  ObjectBackend(const ObjectBackend&) = delete;
  ObjectBackend& operator=(const ObjectBackend&) = delete;

  // 独占打开镜像，先执行恢复，再创建 reader，最后一次性发布可用 Backend。
  static int Open(
      const std::string& image_path, const ObjectBackendOptions& options,
      std::unique_ptr<ObjectBackend>* output,
      journal::RecoveryAction* recovery_action, std::string* detail,
      std::shared_ptr<journal::JournalControlIo> recovery_io = nullptr,
      std::shared_ptr<journal::JournalControlIo> mutation_io = nullptr,
      std::shared_ptr<journal::DurableStageObserver> mutation_observer =
          nullptr);

  // 仅当 name 不存在时原子发布 name+完整 data；存在返回 EEXIST，绝不覆盖。
  int PutIfAbsent(std::string_view name, std::string_view data,
                  std::uint64_t timestamp_ns, std::string* detail);
  // 仅当 expected 与当前对象版本完全一致时，原子替换完整 payload。
  int ReplaceIfVersion(std::string_view name, ObjectVersion expected,
                       std::string_view data, std::uint64_t timestamp_ns,
                       MutationResult* output, std::string* detail);
  // 按对象名读取完整内容；失败时调用者 output 保持原值。
  int Get(std::string_view name, std::string* output, std::string* detail);
  // 只读取对象大小、时间和 generation，不读取 payload。
  int Stat(std::string_view name, ObjectStat* output, std::string* detail);
  // 返回 Backend 是否仍处于可继续服务的确定持久化状态。
  bool usable() const;

 private:
  ObjectBackend(
      ObjectBackendOptions options,
      std::unique_ptr<storage::MountedImageSession> session,
      std::unique_ptr<storage::ImageReader> reader,
      std::shared_ptr<journal::JournalControlIo> mutation_io,
      std::shared_ptr<journal::DurableStageObserver> mutation_observer);

  // 以下 Locked 后缀函数要求调用者已经持有 mutex_。
  int CheckUsableLocked(std::string* detail) const;
  int ResolveRegularLocked(std::string_view name, std::uint32_t* inode_number,
                           ondisk::InodeRecord* inode,
                           std::string* detail) const;
  // 提交 planner 产生的三类块镜像；不确定失败会把 Backend 置为 fail-closed。
  int ApplyLocked(
      const std::map<std::uint32_t, ondisk::Block>& before_images,
      const std::map<std::uint32_t, ondisk::Block>&
          ordered_data_after_images,
      const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
      std::uint32_t total_blocks,
      const std::array<std::uint8_t, 16>& filesystem_uuid,
      MutationOutcome* outcome, std::string* detail);
  // 已提交 metadata 改变后重建 reader；失败同样进入 fail-closed。
  int ReloadReaderLocked(std::string* detail);
  // 销毁 reader 并记录致命根因，永久拒绝当前进程后续请求。
  void FailClosedLocked(int error, std::string_view detail);

  // 配置、镜像所有权、稳定 reader、测试注入点和并发/故障状态。
  ObjectBackendOptions options_;
  std::unique_ptr<storage::MountedImageSession> session_;
  std::unique_ptr<storage::ImageReader> reader_;
  std::shared_ptr<journal::JournalControlIo> mutation_io_;
  std::shared_ptr<journal::DurableStageObserver> mutation_observer_;
  mutable std::mutex mutex_;
  int fatal_error_{0};
  std::string fatal_detail_;
};

}  // namespace eufs::object_store
