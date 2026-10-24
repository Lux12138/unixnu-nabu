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
# The trust cache: launchd is unsigned from the kernel's point of view, and load_static_trust_cache()
# reads /chosen/memory-map/TrustCache and returns early when the segment length is zero, which is exactly
# how we reached "unexpected SIGKILL of initproc (OS_REASON_EXEC / SECURITY_POLICY)".
TRUSTCACHE="$STAGE/EFI/UniXNU/Payloads/kernel.macho.trustcache"
if [ ! -f "$TRUSTCACHE" ] && [ -f /tmp/kernel.macho.trustcache ]; then
  cp /tmp/kernel.macho.trustcache "$TRUSTCACHE"
fi
echo "trustcache: $(stat -c %s "$TRUSTCACHE" 2>/dev/null || echo 0) bytes"

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
#
# rd=md0 is gone on purpose. It told the kernel to take the empty 4 MiB RAM disk as
# /dev/md0, which mockfs then mounted as the root -- so /sbin/launchd was looked for in
# a blank filesystem and every boot ended at "errno 2". The real root is the OS volume
# on UFS, selected through /chosen/root-matching in the ADT. The initrd stays because
# the ADT still declares a RAMDisk node and DarwinBoot validates the two agree.
initrd = "Payloads/ramdisk.img"
# Arguments taken from the documented debugging table in /xnu/boot-arguments, not
# invented here:
#
#   -v serial=3 debug=0x14e   early kernel output
#   kextlog=0xfff             verbose kext logging -- names the kext that stops loading
#   native-ufs-root=1        publish the UFS block class as the root device
#   sheng-data-persist=2     let native_ufs_store_open() wait for the store geometry so the
#                            Data overlay activates and /private/var is writable
#   sheng-smp=0              single CPU; uke found SMP was what triggered its silent resets
#   wdt=-1                   keep the in-kernel watchdog from biting during bring-up
#   sheng-disable-watchdogd=1  stop userland watchdogd from resetting the device
#   native-ufs-allow-unverified=1
#                            adopt the running controller without the verified-root
#                            policy bundle; UniXNU-uke mounts its root this way
#   kt_fixed_zones=120        fixed trace zone count, as sheng uses
#   -no_slto_panic            suppress the SLTO panic
#   amfi_get_out_of_my_way=1  run without AMFI enforcement (diagnostic boot only per the
#                             docs; our payload is unsigned anyway)
#
# boot-progress is deliberately absent: it requires CONFIG_XNU_NATIVE_UFS=1, which
# collides with the IOBlockStorageDevice that apfs pulls in through IOStorageFamily.
args = "-v serial=3 debug=0x14e kextlog=0xfff kt_fixed_zones=120 -no_slto_panic amfi_get_out_of_my_way=1 native-ufs-root=1 native-ufs-allow-unverified=1 sheng-data-persist=2 sheng-smp=0 wdt=-1 sheng-disable-watchdogd=1"
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
