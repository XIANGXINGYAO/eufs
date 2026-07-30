#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
daemon_binary="${2:-./build/eufsd}"
eufsck_binary="${3:-./build/eufsck}"
work_dir="${4:-/tmp/eufs-fuse-write-errors}"
image="$work_dir/eufs.img"
mount_dir="$work_dir/mnt"
log_file="$work_dir/eufsd.log"
state_file="$work_dir/final-state.txt"

mkdir -p "$work_dir" "$mount_dir"

cleanup() {
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${daemon_pid:-}" ]]; then
    wait "$daemon_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

start_daemon() {
  local mode="$1"
  if [[ "$mode" == "readonly" ]]; then
    "$daemon_binary" --image "$image" -f -o ro "$mount_dir" \
      >"$log_file" 2>&1 &
  else
    "$daemon_binary" --image "$image" -f "$mount_dir" \
      >"$log_file" 2>&1 &
  fi
  daemon_pid=$!
  for _ in $(seq 1 100); do
    if mountpoint -q "$mount_dir"; then
      return 0
    fi
    if ! kill -0 "$daemon_pid" 2>/dev/null; then
      wait "$daemon_pid" || true
      cat "$log_file" >&2
      return 1
    fi
    sleep 0.1
  done
  return 1
}

stop_daemon() {
  fusermount3 -u "$mount_dir"
  wait "$daemon_pid"
  unset daemon_pid
}

"$mkfs_binary" --image "$image" --size 1M --inodes 128 \
  --journal-blocks 16 --force >/dev/null

start_daemon writable
python3 - "$image" "$mount_dir/a.txt" "$state_file" <<'PY'
import errno
import hashlib
import os
import sys

image, path, state_file = sys.argv[1:]
block_size = 4096
max_file_size = (12 + block_size // 4) * block_size
block = b'A' * block_size

def digest():
    with open(image, 'rb') as stream:
        return hashlib.sha256(stream.read()).hexdigest()

fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o644)
try:
    assert os.pwrite(fd, block, 0) == len(block)

    before_efbig = digest()
    try:
        os.pwrite(fd, b'X', max_file_size)
        raise AssertionError('maximum-file write unexpectedly succeeded')
    except OSError as error:
        assert error.errno == errno.EFBIG
    assert digest() == before_efbig
    assert os.fstat(fd).st_size == block_size

    existing_blocks = 1
    while True:
        before_enospc = digest()
        try:
            written = os.pwrite(fd, block, existing_blocks * block_size)
        except OSError as error:
            assert error.errno == errno.ENOSPC
            assert digest() == before_enospc
            break
        assert written == len(block)
        existing_blocks += 1

    assert existing_blocks > 12
    full_size = existing_blocks * block_size
    assert os.fstat(fd).st_size == full_size
    assert os.pwrite(fd, b'Z', 0) == 1
    assert os.fstat(fd).st_size == full_size
finally:
    os.close(fd)

with open(path, 'rb') as stream:
    assert stream.read(1) == b'Z'
    assert os.fstat(stream.fileno()).st_size == full_size

with open(state_file, 'w', encoding='ascii') as stream:
    stream.write(f'{existing_blocks} {full_size}\n')
PY
kill -0 "$daemon_pid"
stop_daemon

rg -q 'write plan failed: write exceeds the v1 maximum file size \(errno 27\)' \
  "$log_file"
rg -q 'write plan failed: bitmap has no free allocatable bit \(errno 28\)' \
  "$log_file"
"$eufsck_binary" "$image" >"$work_dir/eufsck.txt"

start_daemon readonly
read -r expected_blocks expected_size <"$state_file"
python3 - "$mount_dir/a.txt" "$expected_size" <<'PY'
import os
import sys

path, expected_size = sys.argv[1], int(sys.argv[2])
with open(path, 'rb') as stream:
    assert stream.read(1) == b'Z'
    assert os.fstat(stream.fileno()).st_size == expected_size
PY
stop_daemon

printf 'PASS: mounted EFBIG/ENOSPC, unchanged failures, post-failure write, eufsck, remount blocks=%s size=%s\n' \
  "$expected_blocks" "$expected_size"
