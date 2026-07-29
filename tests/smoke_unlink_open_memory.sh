#!/usr/bin/env bash
set -euo pipefail

binary="${1:-./build/eufs}"
mount_dir="${2:-/tmp/eufs-unlink-mnt}"
log_file="${3:-/tmp/eufs-unlink.log}"

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
printf 'old-content' >"$mount_dir/a.txt"
exec 3<"$mount_dir/a.txt"

rm "$mount_dir/a.txt"
test ! -e "$mount_dir/a.txt"
listing="$(ls -1A "$mount_dir")"
test "$listing" = $'hello.txt\nnote.txt'

printf 'new-content' >"$mount_dir/a.txt"
test "$(cat "$mount_dir/a.txt")" = "new-content"
old_content=""
IFS= read -r -N 11 -u 3 old_content
test "$old_content" = "old-content"
exec 3<&-

rm "$mount_dir/a.txt"
test ! -e "$mount_dir/a.txt"
test "$(cat "$mount_dir/hello.txt")" = "hello from eufs"
test "$(cat "$mount_dir/note.txt")" = "eufs note"

printf 'PASS: unlink-open-handle smoke test\n'
