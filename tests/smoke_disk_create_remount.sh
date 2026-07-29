#!/usr/bin/env bash
set -euo pipefail

mkfs_binary="${1:-./build/eufs-mkfs}"
daemon_binary="${2:-./build/eufsd}"
image="${3:-/tmp/eufs-disk-create.img}"
mount_dir="${4:-/tmp/eufs-disk-create-mnt}"
log_file="${5:-/tmp/eufs-disk-create.log}"

mkdir -p "$mount_dir"

cleanup() {
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${daemon_pid:-}" ]]; then
    wait "$daemon_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT
umask 022

"$mkfs_binary" --image "$image" --size 64M --inodes 1024 \
  --journal-blocks 256 --force >/dev/null

start_daemon() {
  mode="$1"
  if [[ "$mode" = "readonly" ]]; then
    "$daemon_binary" --image "$image" -f -o ro "$mount_dir" \
      >"$log_file" 2>&1 &
  else
    "$daemon_binary" --image "$image" -f "$mount_dir" \
      >"$log_file" 2>&1 &
  fi
  daemon_pid=$!
  for _ in $(seq 1 50); do
    if mountpoint -q "$mount_dir"; then
      break
    fi
    sleep 0.1
  done
  mountpoint -q "$mount_dir"
}

stop_daemon() {
  fusermount3 -u "$mount_dir"
  wait "$daemon_pid"
  unset daemon_pid
}

start_daemon writable
printf 'hello' >"$mount_dir/a.txt"
test "$(stat -c %i "$mount_dir/a.txt")" = "2"
test "$(stat -c %s "$mount_dir/a.txt")" = "5"
test "$(stat -c %a "$mount_dir/a.txt")" = "644"
test "$(cat "$mount_dir/a.txt")" = "hello"
test "$(ls -1A "$mount_dir")" = "a.txt"
stop_daemon

after_write_digest="$(sha256sum "$image" | cut -d' ' -f1)"

start_daemon readonly
test -f "$mount_dir/a.txt"
test "$(stat -c %i "$mount_dir/a.txt")" = "2"
test "$(stat -c %s "$mount_dir/a.txt")" = "5"
test "$(stat -c %a "$mount_dir/a.txt")" = "644"
test "$(cat "$mount_dir/a.txt")" = "hello"
stop_daemon

after_remount_digest="$(sha256sum "$image" | cut -d' ' -f1)"
test "$after_write_digest" = "$after_remount_digest"

printf 'write_command=printf path=/a.txt inode=2 size=5 data=hello mode=644\n'
printf 'image_sha256_after_write=%s\n' "$after_write_digest"
printf 'image_sha256_after_readonly_remount=%s\n' "$after_remount_digest"
printf 'PASS: FUSE create, first write, and persistent remount smoke test\n'
