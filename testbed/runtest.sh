#!/bin/sh
# usage: runtest.sh <bzImage> <label>
SP=/tmp/claude-1000/-home-claude-Development/0422284e-a96c-49b2-ab6a-740952b67b7f/scratchpad
IMG="$1"; LABEL="$2"
cp -f "$SP/ext4.img" "$SP/ext4-run.img"
timeout 400 qemu-system-x86_64 \
  -kernel "$IMG" \
  -initrd "$SP/initramfs.cpio.gz" \
  -drive file="$SP/ext4-run.img",format=raw,if=virtio \
  -append "console=ttyS0 panic=1 rdinit=/init loglevel=7" \
  -m 2048 -smp 4 -enable-kvm -nographic -no-reboot \
  > "$SP/run-$LABEL.log" 2>&1
echo "=== $LABEL (qemu exit $?) ==="
sed -n '/TESTBED START/,/TESTBED END/p' "$SP/run-$LABEL.log"
