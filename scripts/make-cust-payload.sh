#!/bin/bash
# 鐢熸垚 cust 鍒嗗尯闀滃儚锛?4MB FAT锛夛紝鍐呭惈 UniXNU 閰嶇疆 + XNU 鍐呮牳 + ADT
# 鐢ㄦ硶: bash make-cust-payload.sh <kernel.macho> <kernel.macho.adt> <output.img>
set -e
K="${1:?kernel.macho}"
A="${2:?kernel.macho.adt}"
OUT="${3:-cust-payload.img}"
# Resolve before any cd: a relative $OUT must not resolve inside $STAGE later.
OUT_ABS=$(readlink -f "$OUT")

STAGE=$(mktemp -d)
mkdir -p "$STAGE/EFI/UniXNU/Payloads"
cp "$K" "$STAGE/EFI/UniXNU/Payloads/kernel.macho"
cp "$A" "$STAGE/EFI/UniXNU/Payloads/kernel.macho.adt"

# A 4 MiB RAM disk (the payload volume is 64 MiB and the kernel takes 51): mockfs_mountroot needs a root vnode that answers
# DKIOCGETMEMDEVINFO, and IOMedia only appears for a real device. The contents can be
# empty for a bring-up check -- init will simply fail to find /sbin/launchd afterwards,
# which is the expected next step.
RAMDISK="$STAGE/EFI/UniXNU/Payloads/ramdisk.img"
if [ ! -f "$RAMDISK" ]; then
  dd if=/dev/zero of="$RAMDISK" bs=1M count=4 status=none
  if command -v mkfs.vfat >/dev/null 2>&1; then
    mkfs.vfat -F 16 -n NABURD "$RAMDISK" >/dev/null 2>&1 || true
  fi
fi
echo "ramdisk: $(stat -c %s "$RAMDISK" 2>/dev/null || echo 0) bytes"

cat > "$STAGE/EFI/UniXNU/unixnu.toml" <<'TOML'
version = 1

[boot]
timeout = 5
default = "xnu"

[kits]
autoload = false

[ui]
title = "UniXNU nabu"
logo = true
console = "auto"

[[entry]]
id = "xnu"
title = "XNU (SM8150 UFS)"
type = "xnu"
path = "Payloads/kernel.macho"
# -v/debug=0x14e turn on STARTUP_DEBUG_VERBOSE: kernel_startup_log() then calls
# kprintf once startup_phase passes STARTUP_SUB_KPRINTF, and kprintf before the
# console exists is what stopped the boot at checkpoint 50. Keep the args minimal.
# rd=md0 makes the RAM disk /dev/md0, which mockfs mounts as the root.
initrd = "Payloads/ramdisk.img"
args = "-v serial=3 debug=0x14e -enable_kprintf_spam rd=md0"
TOML

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count=64 status=none
mkfs.vfat -n NABUCUST "$OUT" > /dev/null
( cd "$STAGE" && mcopy -i "$OUT_ABS" -s EFI ::/ )

echo "=== 鍐呭鏍￠獙 ==="
mdir -i "$OUT" -/ ::/EFI/UniXNU/Payloads
ls -l "$OUT"
echo
echo "鍒峰叆: fastboot flash cust $OUT"
rm -rf "$STAGE"
