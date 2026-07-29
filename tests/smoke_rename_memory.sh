#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-rename-mnt}"
log_file="${3:-/tmp/eufs-rename.log}"

mkdir -p "$mount_dir"

cleanup() {
  exec 3<&- 2>/dev/null || true
  fusermount3 -u "$mount_dir" 2>/dev/null || true
  if [[ -n "${eufs_pid:-}" ]]; then
    wait "$eufs_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT

umask 022
"$binary" -f "$mount_dir" >"$log_file" 2>&1 &
eufs_pid=$!

for _ in $(seq 1 50); do
  if mountpoint -q "$mount_dir"; then
    break
  fi
  sleep 0.1
done

mountpoint -q "$mount_dir"
printf 'source-content' >"$mount_dir/a.txt"
printf 'target-content' >"$mount_dir/b.txt"
exec 3<"$mount_dir/b.txt"

mv -f "$mount_dir/a.txt" "$mount_dir/b.txt"
test ! -e "$mount_dir/a.txt"
test "$(cat "$mount_dir/b.txt")" = "source-content"
old_target=""
IFS= read -r -N 14 -u 3 old_target
test "$old_target" = "target-content"
exec 3<&-

mv "$mount_dir/b.txt" "$mount_dir/c.txt"
test ! -e "$mount_dir/b.txt"
test "$(cat "$mount_dir/c.txt")" = "source-content"

printf 'left' >"$mount_dir/a.txt"
printf 'right' >"$mount_dir/b.txt"
mv -n "$mount_dir/a.txt" "$mount_dir/b.txt"
test "$(cat "$mount_dir/a.txt")" = "left"
test "$(cat "$mount_dir/b.txt")" = "right"

printf 'PASS: in-memory rename smoke test\n'
