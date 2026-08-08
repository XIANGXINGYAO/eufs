#pragma once

#include "journal/ondisk_journal.h"
#include "journal/ring_reservation.h"
#include "metadata/ondisk_format.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace eufs::journal {

// 可注入的持久化 I/O 边界，用于故障契约测试；生产环境使用真实 pwrite/fdatasync。
class JournalControlIo {
 public:
  // 基类必须使用虚析构，才能通过基类指针安全销毁测试替身。
  virtual ~JournalControlIo() = default;

  // 可替换的定位写接口；语义与系统调用 pwrite 相同。
  virtual ssize_t Pwrite(int fd, const std::uint8_t* input, std::size_t size,
                         off_t offset) = 0;

  // 可替换的持久化接口；语义与系统调用 fdatasync 相同。
  virtual int Fdatasync(int fd) = 0;
};

// 日志体已经完整写入并持久化、但尚未通过 control 对外暴露时的内存凭证。
struct DurableJournalBody {
  // 记录本事务在 ring 中占用的 descriptor/payload/COMMIT 位置。
  RingReservationPlan reservation;

  // metadata after-image 的数量，也就是 descriptor entry 数量。
  std::uint32_t entry_count{0};

  // descriptor 的 CRC32C，后续 COMMIT 必须绑定同一个 descriptor。
  std::uint32_t descriptor_crc32c{0};
};

// 启动时对 control 当前暴露范围的分类结果，只描述磁盘事实，不执行修改。
enum class RecoveryState {
  // control 表示日志没有待处理事务。
  kEmpty,
  // descriptor/payload 合法，但唯一合法 COMMIT 缺失或无效。
  kUncommitted,
  // descriptor、payload 和 COMMIT 全部通过结构与校验和验证。
  kCommitted,
};

// ResolveRecovery 真正执行恢复后返回给启动层的动作结果。
enum class RecoveryAction {
  // 原本就是干净日志，没有写磁盘。
  kNoAction,
  // 丢弃未提交事务，只推进 control 到干净状态。
  kDiscarded,
  // 回放已提交 metadata after-image，并完成 checkpoint。
  kReplayedAndCheckpointed,
};

// 可观察的六个持久化边界，用于崩溃矩阵而不是业务状态展示。
enum class DurableStage {
  // 新 COW 数据块已经先于元数据落盘。
  kOrderedData,
  // descriptor 和 metadata payload 已经写入 ring 并落盘，但还没暴露。
  kJournalBody,
  // 新 A/B control 已经持久化，恢复程序现在能够看到该事务范围。
  kControlExposure,
  // COMMIT 已经持久化，恢复结论从旧版本切换为新版本。
  kCommit,
  // metadata after-image 已经回放到各自 home block 并落盘。
  kHomeBlocks,
  // control 已清空待处理范围，日志重新成为干净状态。
  kCheckpoint,
};

// 持久化阶段观察接口；生产环境可不提供，测试实现可在指定阶段终止进程。
class DurableStageObserver {
 public:
  // 允许通过基类指针安全销毁具体观察者。
  virtual ~DurableStageObserver() = default;

  // 每完成一个真正持久化的阶段调用一次。
  virtual void OnDurableStage(DurableStage stage) = 0;
};

// 只有 ClassifyRecovery 完整验证成功后才能构造的已提交事务。
// 类型本身把“未经验证的磁盘字节”和“允许回放的事务”区分开。
class ValidatedTransaction {
 public:
  // 验证结果只包含值类型和块副本，因此允许安全复制或移动。
  ValidatedTransaction(const ValidatedTransaction&) = default;
  ValidatedTransaction(ValidatedTransaction&&) = default;
  ValidatedTransaction& operator=(const ValidatedTransaction&) = default;
  ValidatedTransaction& operator=(ValidatedTransaction&&) = default;

  // 返回 descriptor/COMMIT 共同绑定的事务编号。
  std::uint64_t transaction_id() const { return transaction_id_; }

  // 返回 descriptor 在 ring 内的逻辑位置。
  std::uint32_t descriptor_ring_index() const {
    return descriptor_ring_index_;
  }

  // 返回由 descriptor entry 数量唯一推导出的 COMMIT 位置。
  std::uint32_t commit_ring_index() const { return commit_ring_index_; }

  // 返回 home block -> metadata after-image 映射，恢复只能回放这份已验证副本。
  const std::map<std::uint32_t, ondisk::Block>& metadata_after_images() const {
    return metadata_after_images_;
  }

 private:
  // 只有 JournalControlStore 的严格分类器有权填充私有验证结果。
  friend class JournalControlStore;

  // 禁止外部随意构造一个“看起来已验证”的事务。
  ValidatedTransaction() = default;

  // 以下字段只有全部结构检查成功后才会一次性发布。
  std::uint64_t transaction_id_{0};
  std::uint32_t descriptor_ring_index_{0};
  std::uint32_t commit_ring_index_{0};
  std::map<std::uint32_t, ondisk::Block> metadata_after_images_;
};

// 在已经加锁的镜像上管理一次日志事务及恢复状态机。
// 正常写入顺序：ordered data + 未暴露日志体 -> A/B control 暴露事务
// -> COMMIT -> 回放 metadata home blocks -> checkpoint。
// 启动恢复使用同一套格式进行 ClassifyRecovery/ResolveRecovery。
// 中间状态保持为 private，防止调用者跳过持久化阶段或回放未经完整验证的数据。
class JournalControlStore {
 public:
  // 关闭本对象持有的镜像 fd。
  ~JournalControlStore();

  // fd 和事务状态必须只有一个所有者，因此禁止复制。
  JournalControlStore(const JournalControlStore&) = delete;
  JournalControlStore& operator=(const JournalControlStore&) = delete;

  // 独立打开并锁定镜像，再选择有效 A/B control；主要供离线工具和测试使用。
  static int Open(const std::string& image_path,
                  std::unique_ptr<JournalControlStore>* output,
                  std::string* detail,
                  std::shared_ptr<JournalControlIo> io = nullptr,
                  std::shared_ptr<DurableStageObserver> observer = nullptr);

  // 接管调用者已加锁的 fd；无论成功还是校验失败，本函数都会负责关闭该 fd。
  // 在线挂载使用它来复用 FuseMountState 已持有的同一把镜像锁。
  static int AdoptLockedFd(
      int locked_fd, std::unique_ptr<JournalControlStore>* output,
      std::string* detail,
      std::shared_ptr<JournalControlIo> io = nullptr,
      std::shared_ptr<DurableStageObserver> observer = nullptr);

  // 以下只读接口用于查看当前选中的 superblock/control 和事务阶段。
  const ondisk::Superblock& superblock() const { return superblock_; }
  const JournalControl& current() const { return current_; }
  ControlCopy current_copy() const { return current_copy_; }

  // true 表示某次 fdatasync 后状态不确定，必须关闭并重新打开，禁止继续写或恢复。
  bool reload_required() const { return reload_required_; }

  // 日志体持久化成功后存在，control 暴露前后都由状态机继续使用。
  const std::optional<DurableJournalBody>& durable_body() const {
    return durable_body_;
  }

  // true 表示 COMMIT 的 fdatasync 已确认成功。
  bool commit_durable() const { return commit_durable_; }

  // 仅当恢复分类得到 kCommitted 时存在。
  const std::optional<ValidatedTransaction>& validated_transaction() const {
    return validated_transaction_;
  }

  // 在线事务第一步：先持久化 ordered data，再把 metadata after-image 写入
  // 尚未被 control 暴露的 ring 区域。成功后返回 DurableJournalBody 凭证。
  int WriteOrderedDataAndUnexposedBody(
      std::uint32_t expected_total_blocks,
      const std::array<std::uint8_t, 16>& expected_filesystem_uuid,
      const std::map<std::uint32_t, ondisk::Block>& before_images,
      const std::map<std::uint32_t, ondisk::Block>&
          ordered_data_after_images,
      const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
      DurableJournalBody* output, std::string* detail);
  // 只写日志体的底层步骤；调用者必须已经完成 ring 预留和 ordered data 约束。
  int WriteUnexposedBody(
      const RingReservationPlan& reservation,
      const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
      DurableJournalBody* output, std::string* detail);
  // 用下一份 A/B control 持久化事务边界，使恢复程序能够定位 descriptor。
  int ExposeDurableBody(std::string* detail);

  // 在唯一推导位置写入并持久化 COMMIT，建立新版本恢复边界。
  int WriteCommit(std::string* detail);

  // 把已提交 after-image 回放到 home block，持久化后再 checkpoint。
  int CompleteCommittedTransaction(std::string* detail);

  // 只读验证当前暴露事务，分类为空、未提交或已提交，不修改磁盘。
  int ClassifyRecovery(RecoveryState* output, std::string* detail);

  // 根据分类执行丢弃或回放，并最终把 control 推进到干净状态。
  int ResolveRecovery(RecoveryAction* output, std::string* detail);

 private:
  // Open/AdoptLockedFd 完成磁盘校验后使用的私有构造函数。
  JournalControlStore(int fd, const ondisk::Superblock& superblock,
                      const JournalControl& current, ControlCopy current_copy,
                      std::shared_ptr<JournalControlIo> io,
                      std::shared_ptr<DurableStageObserver> observer);

  // 把 next 写入非当前 A/B control copy 并 fdatasync，成功后切换内存 current_。
  int PersistNext(const JournalControl& next, std::string* detail);

  // 本对象独占管理的镜像文件描述符。
  int fd_;

  // 打开时已经完成 CRC 和几何校验的 superblock 副本。
  ondisk::Superblock superblock_;

  // A/B 选择后当前有效的 control 内容。
  JournalControl current_;

  // current_ 来自 A 还是 B，下一次 PersistNext 必须写另一份。
  ControlCopy current_copy_;

  // 实际或测试替代的 pwrite/fdatasync 实现。
  std::shared_ptr<JournalControlIo> io_;

  // 可选的阶段观察者，用于真实进程崩溃注入。
  std::shared_ptr<DurableStageObserver> observer_;

  // 当前在线事务已经持久化的日志体凭证。
  std::optional<DurableJournalBody> durable_body_;

  // 当前在线事务准备回放到 home block 的 metadata after-image。
  std::map<std::uint32_t, ondisk::Block> metadata_after_images_;

  // 启动分类器完整验证后允许恢复执行器回放的事务。
  std::optional<ValidatedTransaction> validated_transaction_;

  // 记录在线事务是否跨过 COMMIT 持久化边界。
  bool commit_durable_{false};

  // 防止同一对象重复执行 checkpoint。
  bool checkpointed_{false};

  // 持久化结果不确定时置 true，阻止基于不可信内存状态继续操作。
  bool reload_required_{false};
};

}  // namespace eufs::journal
