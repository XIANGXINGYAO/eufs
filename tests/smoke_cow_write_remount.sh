#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
daemon_binary="${2:-./build/eufsd}"
eufsck_binary="${3:-./build/eufsck}"
work_dir="${4:-/tmp/eufs-cow-write-remount}"
image="$work_dir/eufs.img"
mount_dir="$work_dir/mnt"
log_file="$work_dir/eufsd.log"

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

"$mkfs_binary" --image "$image" --size 64M --inodes 1024 \
  --journal-blocks 256 --force >/dev/null

start_daemon writable
python3 - "$mount_dir/a.txt" <<'PY'
import os
import sys

path = sys.argv[1]
initial = bytes(ord('A') + index % 17 for index in range(6000))
payload = bytes(ord('k') + index % 17 for index in range(5000))
expected = bytearray(initial)
expected.extend(b'\0' * (8500 - len(expected)))
expected[3500:8500] = payload

fd = os.open(path, os.O_CREAT | os.O_WRONLY, 0o644)
try:
    assert os.write(fd, initial) == len(initial)
    assert os.pwrite(fd, payload, 3500) == len(payload)
    assert os.pwrite(fd, b'TAIL', 8500) == 4
finally:
    os.close(fd)
expected.extend(b'TAIL')
assert open(path, 'rb').read() == expected

thirteen_blocks = b'I' * (13 * 4096)
fd = os.open(path, os.O_WRONLY)
try:
    assert os.pwrite(fd, thirteen_blocks, 0) == len(thirteen_blocks)
finally:
    os.close(fd)
assert open(path, 'rb').read() == thirteen_blocks
PY
stop_daemon

"$eufsck_binary" "$image" >"$work_dir/eufsck.txt"

start_daemon readonly
python3 - "$mount_dir/a.txt" <<'PY'
import sys

contents = open(sys.argv[1], 'rb').read()
assert contents == b'I' * (13 * 4096)
PY
stop_daemon

printf 'PASS: mounted COW overwrite, append, direct/indirect, eufsck, remount\n'
