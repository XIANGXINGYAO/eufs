#!/usr/bin/env bash
set -euo pipefail

vm_dir="${OE_VM_DIR:-$HOME/.local/share/openeuler-vm}"
qemu_root="${OE_QEMU_ROOT:-$HOME/.local/opt/qemu}"
pid_file="$vm_dir/qemu.pid"

if [[ -f "$pid_file" ]] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
  printf 'openEuler VM is already running (PID %s)\n' "$(cat "$pid_file")"
  exit 0
fi

rm -f "$vm_dir/qemu-monitor.sock" "$pid_file"

export LD_LIBRARY_PATH="$qemu_root/usr/lib/x86_64-linux-gnu:$qemu_root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$qemu_root/usr/bin/qemu-system-x86_64" \
  -L "$qemu_root/usr/share/qemu" \
  -machine pc,accel=tcg \
  -cpu max \
  -smp 2 \
  -m 2048 \
  -drive "file=$vm_dir/openeuler-eufs-lab.qcow2,format=qcow2,if=virtio,cache=writeback" \
  -nic user,model=virtio-net-pci,hostfwd=tcp:127.0.0.1:2222-:22 \
  -display none \
  -serial "file:$vm_dir/boot-serial.log" \
  -monitor "unix:$vm_dir/qemu-monitor.sock,server,nowait" \
  -pidfile "$pid_file" \
  -daemonize

printf 'openEuler VM started (PID %s); SSH becomes available at 127.0.0.1:2222 after boot.\n' "$(cat "$pid_file")"
