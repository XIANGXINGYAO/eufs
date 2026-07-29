#!/usr/bin/env bash
set -euo pipefail

vm_dir="${OE_VM_DIR:-$HOME/.local/share/openeuler-vm}"
pid_file="$vm_dir/qemu.pid"
monitor="$vm_dir/qemu-monitor.sock"

if [[ ! -f "$pid_file" ]] || ! kill -0 "$(cat "$pid_file")" 2>/dev/null; then
  rm -f "$pid_file" "$monitor"
  printf 'openEuler VM is not running.\n'
  exit 0
fi

printf 'system_powerdown\n' | nc -U "$monitor" >/dev/null

for _ in $(seq 1 90); do
  if ! kill -0 "$(cat "$pid_file")" 2>/dev/null; then
    rm -f "$pid_file" "$monitor"
    printf 'openEuler VM stopped cleanly.\n'
    exit 0
  fi
  sleep 2
done

printf 'Guest did not stop within 180 seconds; no forced termination was performed.\n' >&2
exit 1
