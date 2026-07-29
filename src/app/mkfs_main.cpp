#include "storage/mkfs.h"

#include <charconv>
#include <cstdint>
#include <getopt.h>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

void PrintUsage(std::ostream& output) {
  output << "Usage: eufs-mkfs --image PATH --size SIZE "
            "[--inodes N] [--journal-blocks N] [--force]\n"
            "SIZE accepts a byte count or K/M/G suffix, for example 64M.\n";
}

bool ParseUnsigned(std::string_view text, std::uint64_t* output) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  *output = value;
  return true;
}

bool ParseSize(std::string_view text, std::uint64_t* output) {
  std::uint64_t multiplier = 1;
  if (!text.empty()) {
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
        break;
    }
  }
  std::uint64_t value = 0;
  if (!ParseUnsigned(text, &value) ||
      value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
    return false;
  }
  *output = value * multiplier;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  eufs::storage::MkfsOptions options;
  bool have_image = false;
  bool have_size = false;

  constexpr option kLongOptions[] = {
      {"image", required_argument, nullptr, 'i'},
      {"size", required_argument, nullptr, 's'},
      {"inodes", required_argument, nullptr, 'n'},
      {"journal-blocks", required_argument, nullptr, 'j'},
      {"force", no_argument, nullptr, 'f'},
      {"help", no_argument, nullptr, 'h'},
      {nullptr, 0, nullptr, 0},
  };

  while (true) {
    const int option_value =
        getopt_long(argc, argv, "i:s:n:j:fh", kLongOptions, nullptr);
    if (option_value == -1) {
      break;
    }
    switch (option_value) {
      case 'i':
        options.image_path = optarg;
        have_image = true;
        break;
      case 's':
        have_size = ParseSize(optarg, &options.image_size_bytes);
        if (!have_size) {
          std::cerr << "invalid --size value\n";
          return 2;
        }
        break;
      case 'n': {
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
        options.force = true;
        break;
      case 'h':
        PrintUsage(std::cout);
        return 0;
      default:
        PrintUsage(std::cerr);
        return 2;
    }
  }

  if (!have_image || !have_size || optind != argc) {
    PrintUsage(std::cerr);
    return 2;
  }

  eufs::ondisk::Superblock superblock;
  std::string error;
  if (!eufs::storage::FormatImage(options, &superblock, &error)) {
    std::cerr << "eufs-mkfs: " << error << '\n';
    return 1;
  }

  std::cout << "formatted " << options.image_path << ": "
            << superblock.total_blocks << " blocks, "
            << superblock.total_inodes << " inodes, data starts at block "
            << superblock.data.start_block << '\n';
  return 0;
}
