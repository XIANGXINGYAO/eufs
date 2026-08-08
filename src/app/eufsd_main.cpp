// 告诉 libfuse 头文件：本程序按照 FUSE 3.1 的接口结构编译。
#define FUSE_USE_VERSION 31

// 这是当前磁盘版 eufsd 的唯一进程入口。
// 启动顺序固定为：解析参数 -> 独占打开并恢复镜像 -> 注册回调 -> 进入 FUSE 事件循环。

#include "fuse/operations.h"
#include "journal/durable_stage_failpoint.h"

#include <fuse3/fuse.h>

#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

// 参数不合法时统一打印命令格式，避免每个分支重复拼接同一段文本。
void PrintUsage() {
  std::cerr << "Usage: eufsd --image IMAGE "
               "[--crash-after STAGE] [FUSE options] MOUNTPOINT\n";
}

}  // 匿名命名空间：上面的辅助符号只在本源文件内可见。

// argc 是参数数量，argv 是参数字符串数组；这是操作系统规定的程序入口签名。
int main(int argc, char** argv) {
  // 保存 --image 指定的 eufs.img 路径；空字符串表示用户还没有提供镜像。
  std::string image_path;

  // 正常运行时为空；指定 --crash-after 后指向上面的进程崩溃注入器。
  std::shared_ptr<eufs::journal::DurableStageObserver> mutation_observer;

  // eufsd 自己消费 --image/--crash-after，其余参数原样转交给 libfuse。
  std::vector<char*> fuse_arguments;

  // 提前预留 argc 个原参数再加一个结尾空指针，避免后续 push_back 反复扩容。
  fuse_arguments.reserve(static_cast<std::size_t>(argc) + 1U);

  // argv[0] 是程序名，libfuse 的参数数组同样要求把它放在第一项。
  fuse_arguments.push_back(argv[0]);

  // 从 argv[1] 开始逐个解析用户参数；argv[0] 不参与选项解析。
  for (int index = 1; index < argc; ++index) {
    // string_view 只引用 argv 中已有字符，不复制参数文本。
    const std::string_view argument(argv[index]);

    // 支持空格形式：--image ./eufs.img。
    if (argument == "--image") {
      // 后面没有路径，或者用户重复指定镜像，都属于参数错误。
      if (index + 1 >= argc || !image_path.empty()) {
        PrintUsage();
        // 返回 2 表示命令行使用错误，而不是文件系统运行失败。
        return 2;
      }
      // ++index 先移动到路径参数，再把路径复制进 image_path。
      image_path = argv[++index];
      // 当前参数及其值已经处理，进入下一轮循环。
      continue;
    }

    // 同时支持等号形式：--image=./eufs.img。
    constexpr std::string_view kImagePrefix = "--image=";
    // 比较参数开头是否为 --image=。
    if (argument.substr(0, kImagePrefix.size()) == kImagePrefix) {
      // 重复指定镜像或等号后为空都不允许。
      if (!image_path.empty() || argument.size() == kImagePrefix.size()) {
        PrintUsage();
        return 2;
      }
      // 去掉 --image= 前缀，只保存真正的镜像路径。
      image_path.assign(argument.substr(kImagePrefix.size()));
      continue;
    }

    // 支持空格形式的崩溃注入参数：--crash-after commit。
    if (argument == "--crash-after") {
      // 必须有阶段名称，并且同一进程只能配置一个崩溃点。
      if (index + 1 >= argc || mutation_observer != nullptr) {
        PrintUsage();
        return 2;
      }
      // 先创建一个待填充的阶段枚举。
      eufs::journal::DurableStage stage{};
      // 消费下一个参数并把字符串解析成阶段枚举。
      if (!eufs::journal::ParseDurableStage(argv[++index], &stage)) {
        PrintUsage();
        return 2;
      }
      // 创建观察者并用 shared_ptr 管理，因为它会传入日志执行链长期持有。
      mutation_observer =
          eufs::journal::MakeProcessCrashObserver(stage, "eufsd");
      continue;
    }

    // 同时支持等号形式：--crash-after=commit。
    constexpr std::string_view kCrashPrefix = "--crash-after=";
    // 判断当前参数是否使用该前缀。
    if (argument.substr(0, kCrashPrefix.size()) == kCrashPrefix) {
      // 保存解析后的崩溃阶段。
      eufs::journal::DurableStage stage{};
      // 拒绝重复配置；再解析等号右侧的阶段名称。
      if (mutation_observer != nullptr ||
          !eufs::journal::ParseDurableStage(
              argument.substr(kCrashPrefix.size()), &stage)) {
        PrintUsage();
        return 2;
      }
      // 解析成功后创建故障注入观察者。
      mutation_observer =
          eufs::journal::MakeProcessCrashObserver(stage, "eufsd");
      continue;
    }

    // 既不是 eufsd 私有参数，就保留给 libfuse，例如 -f、-d 和挂载点。
    fuse_arguments.push_back(argv[index]);
  }

  // 镜像路径是 eufsd 的必需参数，不能依赖 libfuse 替我们检查。
  if (image_path.empty()) {
    PrintUsage();
    return 2;
  }

  // state 独占本次挂载的运行状态；main 不退出，它就不会被析构。
  std::unique_ptr<eufs::fuse_adapter::FuseMountState> state;

  // 接收启动恢复结果，用于记录本次是否丢弃或重放了旧事务。
  eufs::journal::RecoveryAction recovery_action{};

  // 下层失败时把可读错误原因写入 detail。
  std::string detail;

  // 独占打开镜像、执行挂载前恢复、最后创建稳定状态的 ImageReader。
  const int open_result = eufs::fuse_adapter::OpenFuseMountState(
      image_path, &state, &recovery_action, &detail, nullptr,
      std::move(mutation_observer));

  // 任何启动错误都必须在进入 FUSE 事件循环之前终止，不能带病挂载。
  if (open_result != 0) {
    // 内部错误码使用负 errno；显示时取反得到常见正 errno 数字。
    std::cerr << "eufsd: " << detail << " (errno " << -open_result << ")\n";
    // 返回 1 表示程序运行失败。
    return 1;
  }

  // 构造 FUSE 函数指针表，把 getattr/read/write 等请求绑定到我们的实现。
  fuse_operations operations = eufs::fuse_adapter::MakeOperations();

  // fuse_main 需要 int 类型的参数数量；这里记录添加 nullptr 之前的真实数量。
  const int fuse_argc = static_cast<int>(fuse_arguments.size());

  // libfuse 接收传统 argv 数组，最后补 nullptr 作为安全终止标记。
  fuse_arguments.push_back(nullptr);

  // 进入 FUSE 事件循环。state.get() 作为 private_data 传给所有回调；
  // unique_ptr state 仍由 main 持有，因此整个 fuse_main 运行期间该地址始终有效。
  return fuse_main(fuse_argc, fuse_arguments.data(), &operations, state.get());
}
