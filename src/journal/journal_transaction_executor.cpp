#include "journal/journal_transaction_executor.h"

#include <cerrno>
#include <memory>
#include <utility>

namespace eufs::journal {

// 把 planner 生成的块镜像严格按日志协议提交到 session 持有的镜像。
int ExecuteJournalTransaction(
    storage::MountedImageSession& session,
    const std::map<std::uint32_t, ondisk::Block>& before_images,
    const std::map<std::uint32_t, ondisk::Block>&
        ordered_data_after_images,
    const std::map<std::uint32_t, ondisk::Block>& metadata_after_images,
    std::uint32_t total_blocks,
    const std::array<std::uint8_t, 16>& filesystem_uuid,
    std::shared_ptr<JournalControlIo> io,
    std::shared_ptr<DurableStageObserver> observer,
    bool* failure_requires_fail_closed, std::string* detail) {
  // 上层必须接收失败处置结论，否则无法判断是否还能继续服务请求。
  if (failure_requires_fail_closed == nullptr) {
    if (detail != nullptr) {
      detail->assign("journal execution requires a failure disposition output");
    }
    // 空输出指针是调用契约错误，不涉及任何磁盘修改。
    return -EINVAL;
  }
  // 尚未取得事务 fd，当前阶段失败仍可安全返回普通错误。
  *failure_requires_fail_closed = false;

  // 从挂载会话复制一个 fd；复制 fd 继续共享 session 已持有的独占 flock。
  int mutation_fd = -1;
  int result = session.DuplicateFd(&mutation_fd, detail);
  // 复制失败发生在事务存储接管前，没有任何持久化状态变化。
  if (result != 0) {
    return result;
  }

  // 从这里开始，后续步骤可能已经写入但无法确认持久化结果；失败必须 fail-closed。
  *failure_requires_fail_closed = true;

  // JournalControlStore 接管 mutation_fd，并选择当前有效 A/B control。
  std::unique_ptr<JournalControlStore> store;
  result = JournalControlStore::AdoptLockedFd(
      mutation_fd, &store, detail, std::move(io), std::move(observer));
  // 接管或磁盘校验失败时，store 会负责关闭 fd；上层按 fail-closed 处理。
  if (result != 0) {
    return result;
  }

  // 第一步：写新 COW 数据并持久化，再写尚未被 control 暴露的日志体。
  DurableJournalBody body;
  result = store->WriteOrderedDataAndUnexposedBody(
      total_blocks, filesystem_uuid, before_images,
      ordered_data_after_images, metadata_after_images, &body, detail);
  // 第二步：日志体完整持久化后，才允许通过下一份 A/B control 暴露事务范围。
  if (result == 0) {
    result = store->ExposeDurableBody(detail);
  }
  // 第三步：在 descriptor 唯一推导的位置写入并持久化 COMMIT。
  if (result == 0) {
    result = store->WriteCommit(detail);
  }
  // 第四步：COMMIT 后回放 home metadata，并把 control checkpoint 为干净状态。
  if (result == 0) {
    result = store->CompleteCommittedTransaction(detail);
  }
  // 全部阶段成功后，镜像重新处于确定的干净边界，上层无需 fail-closed。
  if (result == 0) {
    *failure_requires_fail_closed = false;
  }
  // 返回第一个失败阶段的负 errno，或成功返回 0。
  return result;
}

}  // namespace eufs::journal
