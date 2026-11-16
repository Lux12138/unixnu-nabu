#!/usr/bin/env python3
"""Execute the production root commands against QEMU's generic UFS model.

No USB, sheng MMIO, target OS, SMMU or guest kernel. Qtest supplies PCI/MMIO
and guest RAM; the compiled production code constructs every UTRD/UPIU/PRDT.
Only a tiny synthetic read-only disk is attached. Output must be outside Git.
"""
import argparse
import ctypes as C
import hashlib
import json
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--output', required=True, type=Path)
p.add_argument('--qemu', default='qemu-system-aarch64')
p.add_argument('--verified-root', type=Path, help='VerifiedRoot checkout (default: umbrella kits/VerifiedRoot)')
p.add_argument('--openssl-prefix', type=Path, help='OpenSSL 3 prefix (default: brew --prefix openssl@3)')
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
source = Path(__file__).resolve().parent
import root_path_fixture
verified_root = (a.verified_root or source.parents[2] / 'kits/VerifiedRoot').resolve()
openssl = a.openssl_prefix or Path(subprocess.check_output(['brew','--prefix','openssl@3'],text=True).strip())
library = root_path_fixture.build_library(source,out,verified_root,openssl)
fixture = root_path_fixture.prepare(out,verified_root)
lib = C.CDLL(str(library))
Callback = C.CFUNCTYPE(C.c_uint64, C.c_uint, C.c_uint64, C.c_uint64, C.c_void_p)
lib.ufs_test_reset.argtypes = [Callback, C.c_uint64]
lib.ufs_test_nop.restype = C.c_uint32
lib.ufs_test_query.argtypes = [C.c_uint8, C.c_uint8, C.POINTER(C.c_uint32)]
lib.ufs_test_query.restype = C.c_uint32
lib.ufs_test_scsi.argtypes = [C.c_uint8,C.c_void_p,C.c_uint,C.c_bool,C.c_uint32,C.POINTER(C.c_uint32)]
lib.ufs_test_scsi.restype = C.c_uint32
lib.ufs_test_read.argtypes = [C.c_uint8,C.c_uint64,C.c_uint32,C.c_void_p]
lib.ufs_test_read.restype = C.c_uint32
lib.ufs_test_poisoned.restype = C.c_bool
lib.ufs_test_store_open.argtypes = [C.c_uint64,C.c_uint64]
lib.ufs_test_store_open.restype = C.c_uint32
lib.ufs_test_log_open.argtypes = [C.c_uint64,C.c_uint64]
lib.ufs_test_log_open.restype = C.c_uint32
lib.ufs_test_write.argtypes = [C.c_uint8,C.c_uint64,C.c_uint32,C.c_void_p]
lib.ufs_test_write.restype = C.c_uint32
lib.ufs_test_sync.restype = C.c_uint32
lib.ufs_test_report.argtypes = [C.c_void_p,C.c_size_t]
lib.ufs_test_stop_queue.restype = C.c_bool
lib.ufs_test_start_queue.restype = C.c_bool

# The stopped CPU never executes this fixture firmware. Its presence enables
# virt's ACPI memory device, allowing 256 MiB at the observed high DMA address
# without reserving tens of GiB of host RAM.
(out / 'blank-firmware.bin').write_bytes(bytes(4096))
pattern = fixture['pattern']
# The write test needs a writable LU; the read-only tests never write, so they run on the same image. Original kept for comparison.
shutil.copyfile(out / 'disk.raw', out / 'disk-rw.raw')
qemu = shutil.which(a.qemu)
assert qemu, 'QEMU unavailable'
version = subprocess.check_output([qemu, '--version'], text=True).splitlines()[0]
results = []
with tempfile.TemporaryDirectory(prefix='ufs-qtest-') as td:
    sock = Path(td) / 'io.sock'
    command = [qemu, '-machine', 'virt,gic-version=3,highmem-ecam=off,acpi=on',
        '-cpu', 'max', '-accel', 'tcg', '-m', '128M,slots=1,maxmem=64G',
        '-bios', str(out / 'blank-firmware.bin'),
        '-object', 'memory-backend-ram,id=hi,size=256M',
        '-device', 'pc-dimm,memdev=hi,addr=0x8c0000000',
        '-device', 'ufs,id=ufs0,bus=pcie.0,addr=1.0',
        '-drive', f'if=none,id=disk,format=raw,file={out / "disk-rw.raw"}',
        '-device', 'ufs-lu,bus=ufs0,drive=disk,lun=0',
        '-nodefaults', '-display', 'none', '-S',
        '-qtest', f'unix:{sock},server=on,wait=off', '-qtest-log', '/dev/null']
    with (out / 'qemu.log').open('w') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 5
            while not sock.exists():
                assert process.poll() is None, (out / 'qemu.log').read_text()
                assert time.monotonic() < deadline, 'Qtest socket startup timeout'
                time.sleep(.01)
            connection = socket.socket(socket.AF_UNIX)
            connection.settimeout(3); connection.connect(str(sock))
            channel = connection.makefile('rwb', buffering=0)

            def qtest(command):
                connection.sendall((command + '\n').encode())
                while True:
                    response = channel.readline().decode().strip()
                    assert response, (out / 'qemu.log').read_text()
                    if response.startswith('IRQ '): continue
                    assert response.startswith('OK'), (command, response)
                    return response[2:].strip()

            def read32(address): return int(qtest(f'readl {address:#x}'), 0)
            def write32(address, value): qtest(f'writel {address:#x} {value:#x}')

            # QEMU virt fixture ECAM, slot 1; never a claimed sheng address.
            pci = 0x3f008000; hc = 0x10000000
            pci_id = read32(pci)
            assert pci_id not in (0, 0xffffffff), 'No UFS PCI device'
            write32(pci + 0x10, hc)
            write32(pci + 0x14, 0)
            qtest(f'writew {pci + 4:#x} 0x6')
            assert read32(hc) & 0x01000000, 'Model lacks 64-bit DMA'
            write32(hc + 0x34, 1)
            write32(hc + 0x90, 0x16)  # Fixture link startup, not production takeover.
            assert read32(hc + 0x30) & 3 == 3
            cap, ver = read32(hc), read32(hc + 8)

            errors = []
            def io(operation, address, value, ptr):
                try:
                    if errors: return 0xffffffffffffffff
                    if operation == 0: return read32(hc + address)
                    if operation == 1: write32(hc + address, value)
                    if operation == 2:
                        data = C.string_at(ptr, value)
                        for at in range(0,value,4096):
                            part = data[at:at+4096]
                            qtest(f'write {address+at:#x} {len(part):#x} 0x{part.hex()}')
                    if operation == 3:
                        data = b''.join(bytes.fromhex(qtest(f'read {address+at:#x} {min(4096,value-at):#x}').removeprefix('0x'))
                                        for at in range(0,value,4096))
                        assert len(data) == value
                        C.memmove(ptr, data, value)
                    if operation == 4: return time.monotonic_ns() // 1000
                    if operation == 5: time.sleep(value / 1000000)
                    return 0
                except BaseException as e:
                    errors.append(repr(e)); return 0xffffffffffffffff
            callback = Callback(io)

            for physical in [0x40010000, 0x8cb5e9000]:
                lib.ufs_test_reset(callback, physical)
                assert lib.ufs_test_stop_queue(), 'Production queue stop failed'
                assert lib.ufs_test_start_queue(), 'Production queue start failed'
                nop = lib.ufs_test_nop()
                if nop:
                    text = C.create_string_buffer(2500); lib.ufs_test_report(text,2500)
                    print(text.value.decode(), flush=True)
                    print('IO errors', errors, flush=True)
                    assert not errors, errors
                    print('UTRD', qtest(f'read {physical:#x} 0x20'), 'RSP',
                          qtest(f'read {physical+4096+512:#x} 0x20'), flush=True)
                assert nop == 0, 'NOP failed'
                value = C.c_uint32()
                assert lib.ufs_test_query(3, 2, C.byref(value)) == 0, 'Power query failed'
                # New QEMU LUs may first return UNIT ATTENTION; allow only a
                # bounded valid CHECK CONDITION before ordinary readiness.
                tur = (C.c_uint8 * 6)(); status = C.c_uint32()
                for attempt in range(3):
                    result = lib.ufs_test_scsi(0, tur, 6, False, 0, C.byref(status))
                    if result == 0: break
                    assert result == 0xe00002d8 and status.value in (2,0x102), (hex(result),hex(status.value),errors)
                assert result == 0, 'TUR did not become ready'
                for blocks in (1, 32):
                    buffer = (C.c_uint8 * (blocks * 4096))()
                    assert lib.ufs_test_read(0, 384, blocks, buffer) == 0, 'READ(10) failed'
                    assert bytes(buffer) == pattern[:len(buffer)], 'DMA payload mismatch'
                path_result = root_path_fixture.check(lib,fixture)
                rejected = (C.c_uint8 * 4096)(*([0xa5] * 4096))
                assert lib.ufs_test_read(0, 0x10000, 1, rejected) != 0, 'Invalid LBA accepted'
                assert bytes(rejected) == bytes([0xa5]) * 4096, 'Error path copied stale payload'
                assert not errors, errors
                # Persistent-store window: nothing is writable until it is opened; writes outside it are refused before any I/O.
                block = (C.c_uint8 * 4096)(*([0x5a] * 4096))
                assert lib.ufs_test_write(0, 448, 1, block) == 0xe00002c2, 'Write accepted before the window was opened'
                assert lib.ufs_test_sync() == 0xe00002d8, 'Sync accepted before the window was opened'
                assert lib.ufs_test_store_open(448, 32) == 1 and lib.ufs_test_store_open(448, 32) == 0, 'Window must open exactly once'
                for lba, blocks in ((447, 1), (479, 2), (480, 1), (0, 1), (8, 1)):
                    assert lib.ufs_test_write(0, lba, blocks, (C.c_uint8 * (4096 * blocks))()) == 0xe00002c2, f'Write outside the window accepted at {lba}'
                assert lib.ufs_test_write(1, 448, 1, block) == 0xe00002c2, 'Write to another LUN accepted'
                wpattern = bytes((i * 7 + physical // 4096 + 3) & 255 for i in range(4096 * 5))
                assert lib.ufs_test_write(0, 449, 5, wpattern) == 0, 'WRITE(10) failed'
                assert lib.ufs_test_sync() == 0, 'SYNCHRONIZE CACHE failed'
                back = (C.c_uint8 * (4096 * 5))()
                assert lib.ufs_test_read(0, 449, 5, back) == 0 and bytes(back) == wpattern, 'Write did not read back'
                assert lib.ufs_test_write(0, 479, 1, block) == 0, 'Last block of the window refused'
                assert lib.ufs_test_write(0, 448, 1, block) == 0, 'First block of the window refused'
                assert lib.ufs_test_sync() == 0
                # Log window: a second window that may not overlap the store window; writes stay inside one of the two.
                assert lib.ufs_test_log_open(470, 16) == 0, 'Log window overlapping the store window accepted'
                assert lib.ufs_test_log_open(480, 16) == 1 and lib.ufs_test_log_open(480, 16) == 0, 'Log window must open exactly once'
                assert lib.ufs_test_write(0, 496, 1, block) == 0xe00002c2, 'Write past the log window accepted'
                assert lib.ufs_test_write(0, 479, 2, (C.c_uint8 * 8192)()) == 0xe00002c2, 'Write across two windows accepted'
                logblock = (C.c_uint8 * 4096)(*([0x6c] * 4096))
                assert lib.ufs_test_write(0, 480, 1, logblock) == 0 and lib.ufs_test_write(0, 495, 1, logblock) == 0, 'Log window refused'
                assert lib.ufs_test_sync() == 0
                assert not lib.ufs_test_poisoned() and not errors, errors
                results.append({'dma': hex(physical), 'nop': 'pass', 'power_query': value.value, 'store_window': 'pass',
                    'root_path': path_result, 'read_4k': 'pass', 'read_128k': 'pass', 'bad_lba_no_copy': 'pass'})
        finally:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
original = (out / 'disk.raw').read_bytes(); after = (out / 'disk-rw.raw').read_bytes()
assert len(original) == len(after)
changed = [i for i in range(len(original) // 4096) if original[i*4096:(i+1)*4096] != after[i*4096:(i+1)*4096]]
assert changed and all(448 <= i < 496 for i in changed), f'Blocks outside the windows changed: {changed}'
assert after[480*4096:481*4096] == bytes([0x6c]) * 4096 and after[495*4096:496*4096] == bytes([0x6c]) * 4096
assert after[479*4096:480*4096] == bytes([0x5a]) * 4096 and after[448*4096:449*4096] == bytes([0x5a]) * 4096
results.append({'image_after': 'only blocks %d..%d of the two windows changed' % (changed[0], changed[-1])})
report = {'qemu': version, 'pci_id': hex(pci_id), 'cap': hex(cap), 'version': hex(ver),
    'results': results, 'source_sha256': {str(f.relative_to(source.parent)): hashlib.sha256(f.read_bytes()).hexdigest()
        for f in [source.parent / 'RootDiagnostics.h', source.parent / 'RootCommands.h', source.parent / 'RootQueue.h', source.parent / 'RootTransportLayout.h', source.parent / 'TransferPoll.h', source / 'RootCommandsBridge.cpp', source / 'RootPathBridge.cpp', source / 'root_path_fixture.py', source.parent / 'RootLayout.h', source.parent / 'RootWindowRead.h']},
    'verified_root_sha256': {name: hashlib.sha256((verified_root/name).read_bytes()).hexdigest() for name in ('src/verified_root.c','tests/openssl_provider.c','include/verified_root.h')},
    'scope': 'Production layout/window/command code, actual portable VerifiedRoot with OpenSSL / generic QEMU UFS DMA; not XNU, CPU cache, SM8550, SMMU or hardware timing'}
(out / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
