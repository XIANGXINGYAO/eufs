#!/usr/bin/env bash
set -euo pipefail

vm_dir="${OE_VM_DIR:-$HOME/.local/share/openeuler-vm}"
ssh_args=(
  -p 2222
  -i "$vm_dir/id_ed25519"
  -o BatchMode=yes
  -o ConnectTimeout=5
  -o GSSAPIAuthentication=no
  -o StrictHostKeyChecking=yes
  -o "UserKnownHostsFile=$vm_dir/known_hosts"
)

echo '== brpc preflight: host backing storage =='
df -hT "$vm_dir"
host_available_kib=$(df -Pk "$vm_dir" | awk 'NR==2 {print $4}')
if (( host_available_kib < 4 * 1024 * 1024 )); then
  echo "FAIL: need at least 4 GiB free on the qcow2 host filesystem, got ${host_available_kib} KiB" >&2
  exit 1
fi

exec ssh "${ssh_args[@]}" root@127.0.0.1 'bash -s' <<'REMOTE_PREFLIGHT'
set -euo pipefail

required_packages=(
  gcc gcc-c++ cmake make ninja-build git python3
  protobuf protobuf-compiler protobuf-devel
  gflags gflags-devel leveldb leveldb-devel
  openssl openssl-devel fuse3 fuse3-devel
)

echo '== brpc preflight: host =='
cat /etc/os-release | sed -n '1,8p'
uname -srmo

echo '== brpc preflight: resources =='
df -hT /
available_kib=$(df -Pk / | awk 'NR==2 {print $4}')
if (( available_kib < 8 * 1024 * 1024 )); then
  echo "FAIL: need at least 8 GiB free on guest root, got ${available_kib} KiB" >&2
  exit 2
fi
free -h
nproc

echo '== brpc preflight: tools =='
for tool in gcc g++ cmake make ninja git python3 protoc pkg-config; do
  command -v "$tool" >/dev/null || {
    echo "FAIL: missing tool $tool" >&2
    exit 3
  }
done
gcc --version | head -1
cmake --version | head -1
protoc --version
ninja --version

echo '== brpc preflight: packages =='
for package in "${required_packages[@]}"; do
  rpm -q "$package" >/dev/null || {
    echo "FAIL: missing RPM package $package" >&2
    exit 4
  }
done

echo '== brpc preflight: pkg-config =='
for module in openssl protobuf gflags leveldb fuse3; do
  version=$(pkg-config --modversion "$module") || {
    echo "FAIL: pkg-config cannot find $module" >&2
    exit 5
  }
  printf '%s %s\n' "$module" "$version"
done

echo '== brpc preflight: protoc and direct C++ link =='
tmp_dir=$(mktemp -d /tmp/brpc-preflight.XXXXXX)
trap 'rm -rf "$tmp_dir"' EXIT

printf '%s\n' \
  'syntax = "proto3";' \
  'package preflight;' \
  'message Probe { string value = 1; }' \
  >"$tmp_dir/probe.proto"

protoc -I"$tmp_dir" --cpp_out="$tmp_dir" "$tmp_dir/probe.proto"

printf '%s\n' \
  '#include <gflags/gflags.h>' \
  '#include <google/protobuf/message.h>' \
  '#include <leveldb/db.h>' \
  '#include <openssl/sha.h>' \
  '#include "probe.pb.h"' \
  'int main() {' \
  '  preflight::Probe probe;' \
  '  probe.set_value("brpc-preflight");' \
  '  leveldb::Options options;' \
  '  options.create_if_missing = false;' \
  '  unsigned char digest[SHA256_DIGEST_LENGTH];' \
  '  SHA256(reinterpret_cast<const unsigned char*>(probe.value().data()), probe.value().size(), digest);' \
  '  return probe.value().empty() || options.create_if_missing ? 1 : 0;' \
  '}' \
  >"$tmp_dir/probe.cc"

g++ -std=c++17 -Wall -Wextra -Werror \
  $(pkg-config --cflags protobuf gflags leveldb openssl) \
  -I"$tmp_dir" \
  "$tmp_dir/probe.cc" "$tmp_dir/probe.pb.cc" \
  -o "$tmp_dir/probe" \
  $(pkg-config --libs protobuf gflags leveldb openssl) -pthread

"$tmp_dir/probe"

echo '== brpc preflight: FUSE headers =='
printf '%s\n' \
  '#define FUSE_USE_VERSION 31' \
  '#include <fuse3/fuse.h>' \
  'int main() { return 0; }' \
  >"$tmp_dir/fuse_probe.cc"
g++ -std=c++17 -Wall -Wextra -Werror \
  $(pkg-config --cflags fuse3) \
  "$tmp_dir/fuse_probe.cc" \
  -o "$tmp_dir/fuse_probe" \
  $(pkg-config --libs fuse3)
"$tmp_dir/fuse_probe"

echo 'PASS: openEuler guest has enough space and all non-brpc build prerequisites pass.'
REMOTE_PREFLIGHT
