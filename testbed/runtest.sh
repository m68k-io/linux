#!/bin/sh
# Boot a kernel under QEMU and run the splice f_pos test matrix.
#
#   ./runtest.sh fixed              # uses images/bzImage-fixed
#   ./runtest.sh baseline
#   ./runtest.sh naive
#   ./runtest.sh /path/to/bzImage mylabel
#
# Results land in run-<label>.log next to this script.
R=$(cd "$(dirname "$0")" && pwd)

case "$1" in
    "")  echo "usage: $0 <baseline|naive|fixed|/path/to/bzImage> [label]"; exit 2 ;;
    /*)  IMG="$1"; LABEL="${2:-custom}" ;;
    *)   IMG="$R/images/bzImage-$1"; LABEL="${2:-$1}" ;;
esac
[ -f "$IMG" ] || { echo "no such image: $IMG"; exit 1; }

cp -f "$R/ext4.img" "$R/ext4-run.img"
timeout 400 qemu-system-x86_64 \
  -kernel "$IMG" \
  -initrd "$R/initramfs.cpio.gz" \
  -drive file="$R/ext4-run.img",format=raw,if=virtio \
  -append "console=ttyS0 panic=1 rdinit=/init loglevel=7" \
  -m 2048 -smp 4 -enable-kvm -nographic -no-reboot \
  > "$R/run-$LABEL.log" 2>&1
rc=$?
echo "=== $LABEL (qemu exit $rc) ==="
sed -n '/TESTBED START/,/TESTBED END/p' "$R/run-$LABEL.log"
