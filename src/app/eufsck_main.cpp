#include "checker/eufsck_command.h"

#include <iostream>
#include <string>
#include <vector>

// 极薄的进程入口：把 argc/argv 转成可测试的 C++ 参数容器，
// 真正的参数解析、扫描和退出码决策全部交给 RunEufsck。
int main(int argc, char** argv) {
  // 不把 argv[0] 程序名传给命令实现。
  std::vector<std::string> arguments;
  // 提前预留实际参数数量，避免循环中重复扩容。
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0U);
  // 复制参数，使命令实现不依赖 argv 生命周期或可写字符数组。
  for (int index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  // 显式传入标准输出/错误流，测试可替换为字符串流验证结果。
  return eufs::checker::RunEufsck(arguments, std::cout, std::cerr);
}
