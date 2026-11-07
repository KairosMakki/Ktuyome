#!/usr/bin/env bash
# Boot Ktuyome under QEMU (ICH9-AHCI SATA by default, legacy IDE optional) and
# verify, from the COM1 serial log, that the disk read/write paths really work:
#
#   - FNV-1a hash of sector 0 as seen by the kernel must match the host's copy
#   - a second read must agree
#   - on a non-KTFS disk: write, read-back-compare, and restore a scratch sector
#
# Usage: tools/qemu-storage-test.sh [ahci|ide]
set -u
cd "$(dirname "$0")/.." || exit 1

MODE="${1:-ahci}"
IMG=build/test-disk.img
LOG=build/serial.log
ARGS=

if [ "$MODE" = "ahci" ]; then
    ARGS="-device ich9-ahci,id=sata -drive file=$IMG,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=sata.0"
else
    ARGS="-drive file=$IMG,if=ide,format=raw"
fi

make >/dev/null 2>&1 || { echo "BUILD FAILED"; exit 1; }
[ -f ktuyome.iso ] || { echo "NO ISO"; exit 1; }

rm -f "$IMG" "$LOG"
dd if=/dev/zero of="$IMG" bs=1M count=64 status=none
dd if=/dev/urandom of="$IMG" bs=512 seek=1 count=1000 conv=notrunc status=none

# Hash the pristine image BEFORE booting: sector 0 is all zeros here (the
# random data goes to sector 1+), and the kernel may rewrite sector 0
# (KTFS first-run format) while the VM is still running, so a post-run
# hash would compare against a mutated image.
HOSTHASH=$(python3 -c "
h = 0x811C9DC5
with open('$IMG', 'rb') as f:
    for b in f.read(512):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
print('%08x' % h)")
# sanity: FNV-1a of 512 zero bytes is 4d7705c5
[ "$HOSTHASH" = "4d7705c5" ] || echo "WARN: unexpected pristine hash $HOSTHASH"

qemu-system-i386 -M pc -m 256 -display none -no-reboot \
    -cdrom ktuyome.iso -serial file:"$LOG" \
    $ARGS >build/qemu.log 2>&1 &
QPID=$!
sleep 60
kill $QPID 2>/dev/null
wait $QPID 2>/dev/null

echo "=== serial log ==="
cat "$LOG"
if [ -s build/qemu.log ]; then echo "=== qemu log ==="; cat build/qemu.log; fi
echo "=== checks ==="
grep -q "S0HASH=0x$HOSTHASH" "$LOG" && echo "PASS: sector-0 hash matches host" || echo "FAIL: sector-0 hash mismatch"
grep -q "READOK" "$LOG" && echo "PASS: double read consistent" || echo "FAIL: double read"
grep -q "WRITEOK" "$LOG" && echo "PASS: write issued" || echo "FAIL: write"
grep -q "WRITECMP-OK" "$LOG" && echo "PASS: write readback compare" || echo "FAIL: write compare"
grep -q "RESTORED" "$LOG" && echo "PASS: scratch sector restored" || echo "FAIL: restore"
if [ "$MODE" = "ahci" ]; then
    grep -q "DISK: ahci" "$LOG" && echo "PASS: AHCI driver active" || echo "FAIL: AHCI not active"
else
    grep -q "DISK: ata-pio" "$LOG" && echo "PASS: ATA PIO fallback active" || echo "FAIL: ATA fallback"
fi
