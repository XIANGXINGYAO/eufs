#pragma once

#include "journal/journal_control_store.h"
#include "storage/image_reader.h"
#include "storage/mounted_image_session.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace eufs::fuse_adapter {

// 一次磁盘版 FUSE 挂载共享的全部运行状态。
// fuse_main 把该对象的非拥有指针保存为 private_data，所有回调再通过
// fuse_get_context() 取回同一个对象。
struct FuseMountState {
  // 构造函数接管镜像会话、reader 和可选故障注入观察者的所有权。
  FuseMountState(std::unique_ptr<storage::MountedImageSession> session_value,
                 std::unique_ptr<storage::ImageReader> reader_value,
                 std::shared_ptr<journal::DurableStageObserver>
                     mutation_observer_value = nullptr);

  // 会话中含有文件描述符、独占锁和 mutex，复制会破坏唯一所有权，所以明确禁用。
  FuseMountState(const FuseMountState&) = delete;
  FuseMountState& operator=(const FuseMountState&) = delete;

  // 只有未发生致命错误，并且 session/reader 都存在时才能继续服务请求。
  bool usable() const;

  // 已提交事务修改 home metadata 后重建 reader。
  // 如果重建失败，继续使用旧 reader 会暴露过期状态，因此失败必须关闭服务能力。
  int ReloadReader(std::string* detail);

  // 持久化结果不确定时进入 fail-closed：销毁 reader，并永久拒绝后续请求。
  // 只有重启并重新执行挂载前恢复，才能重新建立可信磁盘边界。
  void FailClosed(int error, std::string_view detail);

  // 只读暴露首次致命错误码。
  int fatal_error() const { return fatal_error_; }

  // 只读暴露首次致命错误的上下文文本。
  const std::string& fatal_detail() const { return fatal_detail_; }

  // 持有 eufs.img 文件描述符及独占 flock，保证同一镜像只有一个在线写者。
  std::unique_ptr<storage::MountedImageSession> session;

  // 持有最近一次稳定磁盘状态的解析视图，负责路径、inode 和数据块读取。
  std::unique_ptr<storage::ImageReader> reader;

  // 测试专用持久化阶段观察者；正常运行时为空。
  std::shared_ptr<journal::DurableStageObserver> mutation_observer;

  // 串行化一次完整请求：读状态、生成计划、提交、回放、checkpoint、重建 reader。
  std::mutex mutex;

 private:
  // 0 表示可用；非 0 保存使挂载进入 fail-closed 的负 errno。
  int fatal_error_{0};

  // 保存 fatal_error_ 对应的详细故障原因。
  std::string fatal_detail_;
};

// 建立可供 fuse_main 使用的挂载状态。
// 函数必须先独占打开镜像并完成恢复，再创建 reader，绝不能暴露恢复中间状态。
int OpenFuseMountState(
    const std::string& image_path, std::unique_ptr<FuseMountState>* output,
    journal::RecoveryAction* recovery_action, std::string* detail,
    std::shared_ptr<journal::JournalControlIo> recovery_io = nullptr,
    std::shared_ptr<journal::DurableStageObserver> mutation_observer = nullptr);

}  // namespace eufs::fuse_adapter
