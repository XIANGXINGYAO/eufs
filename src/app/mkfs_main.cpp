#include "storage/mkfs.h"

#include <charconv>
#include <cstdint>
#include <getopt.h>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

// 输出 eufs-mkfs 支持的命令行格式；参数决定写到 stdout 还是 stderr。
void PrintUsage(std::ostream& output) {
  output << "Usage: eufs-mkfs --image PATH --size SIZE "
            "[--inodes N] [--journal-blocks N] [--force]\n"
            "SIZE accepts a byte count or K/M/G suffix, for example 64M.\n";
}

// 严格解析无符号十进制整数：不允许空串、负号、尾随字符或溢出。
bool ParseUnsigned(std::string_view text, std::uint64_t* output) {
  // 空字符串没有可解析数字。
  if (text.empty()) {
    return false;
  }
  // 使用局部变量，解析失败时不污染调用者 output。
  std::uint64_t value = 0;
  // from_chars 不分配内存，也不受 locale 影响，适合命令行数值解析。
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  // ec 必须成功，并且 ptr 必须到达末尾，才能拒绝 12abc 这类部分解析。
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  // 全部检查通过后才发布解析值。
  *output = value;
  return true;
}

// 解析镜像容量，支持纯字节数以及 K/M/G 二进制倍率后缀。
bool ParseSize(std::string_view text, std::uint64_t* output) {
  // 没有后缀时倍率为 1，也就是直接按字节解释。
  std::uint64_t multiplier = 1;
  if (!text.empty()) {
    // 只检查最后一个字符，识别后将它从待解析数字中移除。
    switch (text.back()) {
      case 'K':
      case 'k':
        multiplier = 1024ULL;
        text.remove_suffix(1);
        break;
      case 'M':
      case 'm':
        multiplier = 1024ULL * 1024ULL;
        text.remove_suffix(1);
        break;
      case 'G':
      case 'g':
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        text.remove_suffix(1);
        break;
      default:
        // 不是已知后缀时保留原字符串，交给 ParseUnsigned 严格判断。
        break;
    }
  }
  // 先解析去掉后缀后的数字主体。
  std::uint64_t value = 0;
  // 乘法前显式检查上溢，避免容量回绕成较小值。
  if (!ParseUnsigned(text, &value) ||
      value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return false;
  }
  // 只有解析与溢出检查都成功后才写回最终字节数。
  *output = value * multiplier;
  return true;
}

}  // 匿名命名空间：参数解析辅助函数不导出到其他目标。

// eufs-mkfs 进程入口：解析参数，调用 FormatImage，最后输出生成的磁盘几何。
int main(int argc, char** argv) {
  // 保存传给真正格式化逻辑的全部选项及默认值。
  eufs::storage::MkfsOptions options;
  // 单独记录两个必选项是否出现，不能用默认空值代替“用户已提供”。
  bool have_image = false;
  bool have_size = false;

  // getopt_long 使用的长选项表；最后一项全零作为结束标记。
  constexpr option kLongOptions[] = {
      {"image", required_argument, nullptr, 'i'},
      {"size", required_argument, nullptr, 's'},
      {"inodes", required_argument, nullptr, 'n'},
      {"journal-blocks", required_argument, nullptr, 'j'},
      {"force", no_argument, nullptr, 'f'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  // getopt_long 每次返回一个短选项字符，-1 表示参数扫描结束。
  while (true) {
    const int option_value =
        getopt_long(argc, argv, "i:s:n:j:fh", kLongOptions, nullptr);
    if (option_value == -1) {
      break;
    }
    // 按选项类型填充 MkfsOptions，任何非法值都立即返回命令行错误 2。
    switch (option_value) {
      case 'i':
        // optarg 由 getopt_long 指向 --image 的参数文本。
        options.image_path = optarg;
        have_image = true;
        break;
      case 's':
        // 容量支持 64M 等后缀形式，解析结果直接写入字节数。
        have_size = ParseSize(optarg, &options.image_size_bytes);
        if (!have_size) {
          std::cerr << "invalid --size value\n";
          return 2;
        }
        break;
      case 'n': {
        // inode 数先用 uint64_t 接收，再检查能否安全缩窄为磁盘字段 uint32_t。
        std::uint64_t value = 0;
        if (!ParseUnsigned(optarg, &value) || value == 0 ||
            value > std::numeric_limits<std::uint32_t>::max()) {
          std::cerr << "invalid --inodes value\n";
          return 2;
        }
        options.total_inodes = static_cast<std::uint32_t>(value);
        break;
      }
      case 'j': {
        // journal 块数还必须满足格式协议规定的最小容量。
        std::uint64_t value = 0;
        if (!ParseUnsigned(optarg, &value) ||
            value < eufs::ondisk::kMinimumJournalBlocks ||
            value > std::numeric_limits<std::uint32_t>::max()) {
          std::cerr << "invalid --journal-blocks value\n";
          return 2;
        }
        options.journal_blocks = static_cast<std::uint32_t>(value);
        break;
      }
      case 'f':
        // 允许覆盖已经存在的镜像路径；实际安全检查由 FormatImage 完成。
        options.force = true;
        break;
      case 'h':
        // 主动请求帮助属于成功退出。
        PrintUsage(std::cout);
        return 0;
      default:
        // 未识别选项或缺失参数由 getopt_long 落入这里。
        PrintUsage(std::cerr);
        return 2;
    }
  }

  // 两个必选项必须存在，并且不能残留未被 getopt 消费的位置参数。
  if (!have_image || !have_size || optind != argc) {
    PrintUsage(std::cerr);
    return 2;
  }

  // FormatImage 成功时返回经过编码的 superblock，失败原因写入 error。
  eufs::ondisk::Superblock superblock;
  std::string error;
  if (!eufs::storage::FormatImage(options, &superblock, &error)) {
    std::cerr << "eufs-mkfs: " << error << '\n';
    return 1;
  }

  // 输出实际落盘后的关键几何，方便用户核对总块数、inode 数和数据区起点。
  std::cout << "formatted " << options.image_path << ": "
            << superblock.total_blocks << " blocks, "
            << superblock.total_inodes << " inodes, data starts at block "
            << superblock.data.start_block << '\n';
  return 0;
}
