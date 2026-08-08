#include "journal/journal_control_store.h"

// 对持久化 A/B control 选定的事务范围执行恢复分类和恢复动作。
// 恢复器绝不扫描 ring 寻找“看起来合法”的 COMMIT：descriptor 必须唯一推导出
// 每个 payload 位置和唯一 COMMIT 位置。结构矛盾返回 EUCLEAN；合法 COMMIT
// 缺失时只把已暴露事务判定为未提交并丢弃。

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unistd.h>

namespace eufs::journal {
namespace {

// detail 是可选输出参数；调用者不关心文本时可以传 nullptr。
void SetDetail(std::string* detail, std::string_view message) {
  // 只有调用者提供了字符串地址才写入，避免解引用空指针。
  if (detail != nullptr) {
    detail->assign(message);
  }
}

// 统一生成“磁盘结构损坏”错误，避免不同检查点返回不同 errno。
int Corrupt(std::string* detail, std::string_view message) {
  SetDetail(detail, message);
  // EUCLEAN 表示镜像能读取，但内部结构不满足文件系统约束。
  return -EUCLEAN;
}

// 带下层具体原因的损坏错误，例如 descriptor 解码失败的字段原因。
int Corrupt(std::string* detail, std::string_view context,
            const std::string& cause) {
  if (detail != nullptr) {
    // 先写当前恢复步骤，再追加底层解码器提供的具体原因。
    detail->assign(context);
    detail->append(": ");
    detail->append(cause);
  }
  return -EUCLEAN;
}

// pread 允许短读和 EINTR；恢复代码必须循环到完整读取 size 字节。
int PreadAll(int fd, std::uint8_t* output, std::size_t size,
             std::uint64_t offset, std::string_view operation,
             std::string* detail) {
  // completed 表示已经成功放入 output 的字节数。
  std::size_t completed = 0;
  while (completed < size) {
    // 每轮从尚未读取的偏移继续，不能在短读后从头覆盖。
    const auto result = pread(fd, output + completed, size - completed,
                              static_cast<off_t>(offset + completed));
    // 信号中断不代表 I/O 失败，保持 completed 不变并重试。
    if (result < 0 && errno == EINTR) {
      continue;
    }
    // 负数是真实系统错误；0 表示镜像意外提前结束，同样按 EIO 处理。
    if (result <= 0) {
      const int error_number = result < 0 ? errno : EIO;
      if (detail != nullptr) {
        detail->assign(operation);
        detail->append(": ");
        detail->append(std::strerror(error_number));
      }
      // 项目内部统一返回负 errno。
      return -error_number;
    }
    // 正常短读时累计进度，下一轮读取剩余部分。
    completed += static_cast<std::size_t>(result);
  }
  // 只有完整读满请求区域才返回成功。
  return 0;
}

// 与 PreadAll 对称：循环处理短写和 EINTR，保证完整写入一个 after-image。
int PwriteAll(JournalControlIo* io, int fd, const std::uint8_t* input,
              std::size_t size, std::uint64_t offset,
              std::string_view operation, std::string* detail) {
  // 记录已经成功提交给内核的字节数。
  std::size_t completed = 0;
  while (completed < size) {
    // 清空旧 errno，防止测试替身返回异常值时误读上一次系统调用错误。
    errno = 0;
    const auto result = io->Pwrite(
        fd, input + completed, size - completed,
        static_cast<off_t>(offset + completed));
    // EINTR 时不改变进度，重新执行剩余写入。
    if (result < 0 && errno == EINTR) {
      continue;
    }
    // 负数或零长度都无法推进写入，必须失败退出。
    if (result <= 0) {
      const int error_number = result < 0 && errno != 0 ? errno : EIO;
      if (detail != nullptr) {
        detail->assign(operation);
        detail->append(": ");
        detail->append(std::strerror(error_number));
      }
      return -error_number;
    }
    // 防御错误 I/O 实现：返回值不能大于本轮请求长度。
    if (static_cast<std::size_t>(result) > size - completed) {
      SetDetail(detail, "pwrite returned an invalid length");
      return -EIO;
    }
    // 正常短写时累计进度。
    completed += static_cast<std::size_t>(result);
  }
  return 0;
}

// 在 ring 内向前移动 distance 个块；取模负责尾部回绕。
std::uint32_t Advance(std::uint32_t start, std::size_t distance,
                      std::uint32_t ring_blocks) {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(start) + distance) % ring_blocks);
}

// 判断物理块号是否落在一个连续磁盘区域内。
bool BlockIsInRegion(std::uint32_t block, const ondisk::Region& region) {
  return block >= region.start_block &&
         static_cast<std::uint64_t>(block) <
             static_cast<std::uint64_t>(region.start_block) +
                 region.block_count;
}

// metadata home block 必须非 0、位于镜像内，并且不能指回 journal 自身。
bool IsValidMetadataTarget(const ondisk::Superblock& superblock,
                           std::uint32_t block) {
  return block != 0 && block < superblock.total_blocks &&
         !BlockIsInRegion(block, superblock.journal);
}

// 把 journal ring 逻辑下标转换成镜像物理块号并完整读取一个块。
int ReadRingBlock(int fd, const ondisk::Superblock& superblock,
                  std::uint32_t ring_index, ondisk::Block* output,
                  std::string_view operation, std::string* detail) {
  // journal 区域开头先放 A/B control，剩余块才组成循环 ring。
  const std::uint32_t ring_blocks =
      superblock.journal.block_count - ondisk::kJournalControlBlockCount;
  // 输出地址为空或逻辑下标越界都说明恢复推导不可信。
  if (output == nullptr || ring_index >= ring_blocks) {
    return Corrupt(detail, "derived journal position is outside the ring");
  }
  // ring 下标 0 对应两个 control block 之后的第一个物理块。
  const std::uint64_t physical_block =
      static_cast<std::uint64_t>(superblock.journal.start_block) +
      ondisk::kJournalControlBlockCount + ring_index;
  // 计算 journal 区域右开边界，用于防止整数或几何错误越界读取。
  const std::uint64_t journal_end =
      static_cast<std::uint64_t>(superblock.journal.start_block) +
      superblock.journal.block_count;
  // 同时检查不能越过 journal，也不能越过整个镜像。
  if (physical_block >= journal_end ||
      physical_block >= superblock.total_blocks) {
    return Corrupt(detail, "derived journal position maps outside the image");
  }
  // 块号转换为字节偏移后读取固定 4096 字节 ondisk::Block。
  return PreadAll(fd, output->data(), output->size(),
                  physical_block * ondisk::kBlockSize, operation, detail);
}

}  // 匿名命名空间：辅助函数只供本恢复实现使用。

// 只读取和验证当前 control 暴露的事务，不修改任何磁盘块。
int JournalControlStore::ClassifyRecovery(RecoveryState* output,
                                          std::string* detail) {
  // 分类结果是必需输出，空指针属于调用错误而不是磁盘损坏。
  if (output == nullptr) {
    SetDetail(detail, "recovery state output is required");
    return -EINVAL;
  }
  // 新一次分类开始前清空旧错误文本。
  if (detail != nullptr) {
    detail->clear();
  }

  // 先销毁上次验证结果；本次任何中途失败都不能留下可回放的旧事务。
  validated_transaction_.reset();
  // fdatasync 结果不确定时，当前内存 control 可能已经落后，必须重新打开重选 A/B。
  if (reload_required_) {
    SetDetail(detail,
              "journal durability is uncertain; reopen and reselect before "
              "recovery classification");
    return -EIO;
  }
  // used_blocks 为 0 表示 control 没有暴露任何待处理事务。
  if (current_.used_blocks == 0) {
    *output = RecoveryState::kEmpty;
    return 0;
  }
  // 非空事务的 id 应为 next_transaction_id - 1，因此 next 至少为 2。
  if (current_.next_transaction_id <= 1) {
    return Corrupt(detail,
                   "nonempty journal control has no preceding transaction id");
  }

  // control.tail 按协议必须指向 descriptor。
  ondisk::Block descriptor_bytes{};
  int result = ReadRingBlock(fd_, superblock_, current_.tail,
                             &descriptor_bytes, "pread journal descriptor",
                             detail);
  // 读取失败是 I/O 错误，不能继续猜测后续位置。
  if (result != 0) {
    return result;
  }

  // 把固定宽度磁盘字节解码为有字段语义的 descriptor。
  DescriptorRecord descriptor;
  std::string decode_error;
  // magic、版本、entry 数量、CRC 等任一规则失败都判定结构损坏。
  if (!DecodeDescriptor(descriptor_bytes, &descriptor, &decode_error)) {
    return Corrupt(detail, "exposed descriptor is invalid", decode_error);
  }
  // control 保存的是“下一事务 id”，当前暴露事务应当恰好小一。
  const std::uint64_t expected_transaction_id =
      current_.next_transaction_id - 1U;
  // UUID 防止把另一镜像遗留的日志错误回放到当前文件系统。
  if (descriptor.filesystem_uuid != superblock_.filesystem_uuid) {
    return Corrupt(detail, "descriptor UUID does not match the image");
  }
  // descriptor 事务号必须与 control 声明的当前事务一致。
  if (descriptor.transaction_id != expected_transaction_id) {
    return Corrupt(detail,
                   "descriptor transaction id does not match the control");
  }

  // 每个 descriptor entry 对应一个 metadata payload。
  const std::size_t payload_count = descriptor.entries.size();
  // 事务占用块数必须等于 descriptor + payload + COMMIT 的协议长度。
  if (descriptor.transaction_block_count != current_.used_blocks) {
    return Corrupt(detail,
                   "descriptor length does not match the exposed range");
  }
  // 逐条检查 payload 位置和最终回放目标，但此时还不读取 payload 内容。
  for (std::size_t index = 0; index < payload_count; ++index) {
    const auto& entry = descriptor.entries[index];
    // 第 index 个 payload 必须紧跟 descriptor 顺序排列，允许 ring 尾部回绕。
    const std::uint32_t expected_position =
        Advance(current_.tail, index + 1U, current_.ring_blocks);
    if (entry.payload_ring_index != expected_position) {
      return Corrupt(detail,
                     "descriptor payload position violates the v1 grammar");
    }
    // home block 不能是 0、镜像外块或 journal 内部块。
    if (!IsValidMetadataTarget(superblock_, entry.home_block)) {
      return Corrupt(detail,
                     "descriptor targets a forbidden metadata home block");
    }
  }

  // 唯一合法 COMMIT 位于全部 payload 之后。
  const std::uint32_t commit_ring_index =
      Advance(current_.tail, payload_count + 1U, current_.ring_blocks);
  // head 必须位于 COMMIT 之后；由 descriptor 独立推导，不能盲信 control.head。
  const std::uint32_t derived_head =
      Advance(current_.tail, payload_count + 2U, current_.ring_blocks);
  // 推导终点与 control 声明不一致说明事务边界自相矛盾。
  if (derived_head != current_.head) {
    return Corrupt(detail,
                   "descriptor transaction end does not match control head");
  }

  // candidate 先在局部变量中构造；全部验证前不发布到成员变量。
  ValidatedTransaction candidate;
  candidate.transaction_id_ = descriptor.transaction_id;
  candidate.descriptor_ring_index_ = current_.tail;
  candidate.commit_ring_index_ = commit_ring_index;
  // 读取每个 payload，验证 CRC 后按 home block 收集 after-image。
  for (const auto& entry : descriptor.entries) {
    ondisk::Block payload{};
    result = ReadRingBlock(fd_, superblock_, entry.payload_ring_index, &payload,
                           "pread journal payload", detail);
    // payload I/O 失败立即停止，本次没有可用验证结果。
    if (result != 0) {
      return result;
    }
    // descriptor 保存的 CRC 必须与真实 payload 字节一致。
    if (ondisk::Crc32c(payload.data(), payload.size()) !=
        entry.payload_crc32c) {
      return Corrupt(detail, "exposed journal payload checksum mismatch");
    }
    // CRC 通过后才把 payload 加入候选回放集合。
    candidate.metadata_after_images_.emplace(entry.home_block,
                                              std::move(payload));
  }

  // 在唯一推导位置读取 COMMIT，不扫描其他 ring block。
  ondisk::Block commit_bytes{};
  result = ReadRingBlock(fd_, superblock_, commit_ring_index, &commit_bytes,
                         "pread journal COMMIT", detail);
  // 读错误和“没有合法 COMMIT”不同；前者不能被当作未提交成功处理。
  if (result != 0) {
    return result;
  }

  // 解码 COMMIT，并清除 descriptor 解码阶段留下的临时错误文本。
  CommitRecord commit;
  decode_error.clear();
  // 无法解码或事务号不同都表示该事务尚未形成合法提交边界。
  if (!DecodeCommit(commit_bytes, &commit, &decode_error) ||
      commit.transaction_id != descriptor.transaction_id) {
    // 未提交是可恢复状态，不是 EUCLEAN；后续 ResolveRecovery 会清空 control。
    *output = RecoveryState::kUncommitted;
    return 0;
  }
  // 合法 COMMIT 还必须绑定同一 descriptor CRC、位置和事务长度。
  if (!CommitMatchesDescriptor(descriptor, current_.tail, commit,
                               &decode_error)) {
    return Corrupt(detail, "current COMMIT is inconsistent", decode_error);
  }

  // 所有验证通过后才原子发布 candidate，允许 ResolveRecovery 使用。
  validated_transaction_ = std::move(candidate);
  *output = RecoveryState::kCommitted;
  return 0;
}

// 执行启动恢复：空日志不动，未提交事务丢弃，已提交事务回放并 checkpoint。
int JournalControlStore::ResolveRecovery(RecoveryAction* output,
                                         std::string* detail) {
  // 恢复动作结果是必需输出。
  if (output == nullptr) {
    SetDetail(detail, "recovery action output is required");
    return -EINVAL;
  }
  // 开始前清空旧错误文本。
  if (detail != nullptr) {
    detail->clear();
  }
  // 持久化不确定时必须重新打开，不能在可能过期的 current_ 上恢复。
  if (reload_required_) {
    SetDetail(detail,
              "journal durability is uncertain; reopen and reselect before "
              "recovery execution");
    return -EIO;
  }
  // 本函数只允许刚打开的 store 调用，不能混入正在进行的在线事务。
  if (durable_body_.has_value() || commit_durable_) {
    SetDetail(detail,
              "recovery execution requires a newly opened control store");
    return -EBUSY;
  }

  // 先完整分类；任何结构矛盾或 I/O 错误都阻止恢复执行。
  RecoveryState state{};
  int result = ClassifyRecovery(&state, detail);
  if (result != 0) {
    return result;
  }
  // 空日志不需要产生任何磁盘写入。
  if (state == RecoveryState::kEmpty) {
    *output = RecoveryAction::kNoAction;
    return 0;
  }

  // 构造下一份干净 control：代次加一、tail 追到 head、占用块数清零。
  JournalControl clean = current_;
  clean.generation += std::uint64_t{1};
  clean.tail = clean.head;
  clean.used_blocks = 0;
  // checksum 在 PersistNext 编码时重新计算，内存候选先清零。
  clean.checksum = 0;

  // 未提交事务没有资格回放 payload，只需持久化干净 control 丢弃其范围。
  if (state == RecoveryState::kUncommitted) {
    result = PersistNext(clean, detail);
    // control 持久化失败时不能声称已经丢弃。
    if (result != 0) {
      return result;
    }
    // 防御性清空验证结果，然后报告本次执行了丢弃。
    validated_transaction_.reset();
    *output = RecoveryAction::kDiscarded;
    return 0;
  }

  // kCommitted 必须伴随 ClassifyRecovery 发布的完整验证事务。
  if (!validated_transaction_.has_value()) {
    return Corrupt(detail,
                   "committed classification omitted its validated transaction");
  }
  // 按 home block 顺序回放所有 metadata after-image。
  for (const auto& [home_block, after_image] :
       validated_transaction_->metadata_after_images()) {
    // 即使分类阶段检查过，执行前仍防御验证对象被错误构造或破坏。
    if (!IsValidMetadataTarget(superblock_, home_block)) {
      return Corrupt(detail,
                     "validated transaction contains a forbidden home block");
    }
    // 把一个完整 metadata block 定位写回其 home 位置。
    result = PwriteAll(io_.get(), fd_, after_image.data(), after_image.size(),
                       static_cast<std::uint64_t>(home_block) *
                           ondisk::kBlockSize,
                       "pwrite recovery home block", detail);
    // 部分 home block 可能已写入，失败后磁盘处于未知混合状态，必须要求重开恢复。
    if (result != 0) {
      reload_required_ = true;
      return result;
    }
  }

  // 清空旧 errno，随后一次 fdatasync 确认全部 home block 已持久化。
  errno = 0;
  if (io_->Fdatasync(fd_) != 0) {
    const int error_number = errno != 0 ? errno : EIO;
    if (detail != nullptr) {
      detail->assign("fdatasync recovery home blocks: ");
      detail->append(std::strerror(error_number));
    }
    // sync 失败意味着不能确认哪些 home block 持久化，禁止在本对象上继续。
    reload_required_ = true;
    return -error_number;
  }

  // home blocks 确认落盘后，最后持久化干净 control 完成 checkpoint。
  result = PersistNext(clean, detail);
  if (result != 0) {
    return result;
  }
  // checkpoint 成功后不再需要保留已验证事务副本。
  validated_transaction_.reset();
  *output = RecoveryAction::kReplayedAndCheckpointed;
  return 0;
}

}  // namespace eufs::journal
