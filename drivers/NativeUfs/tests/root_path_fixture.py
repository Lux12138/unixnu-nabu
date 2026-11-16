# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Small synthetic signed windows for the independent QEMU UFS test.

Fixture keys stay in the caller's ignored output directory. No target image,
user key, USB, guest CPU execution or root-acceptance claim is involved.
"""
import ctypes as C
import hashlib
import importlib.util
import io
import os
from pathlib import Path
import struct
import subprocess
import uuid
import zlib


def build_library(source, out, verified_root, openssl):
    objects = []
    for name in ('src/verified_root.c', 'tests/openssl_provider.c'):
        target = out / (Path(name).stem + '.o')
        subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fPIC',
            '-I', str(verified_root / 'include'), '-I', str(openssl / 'include'),
            '-c', str(verified_root / name), '-o', str(target)], check=True)
        objects.append(str(target))
    library = out / 'root-path.dylib'
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-dynamiclib',
        '-I', str(verified_root / 'include'), '-I', str(openssl / 'include'),
        str(source / 'RootPathBridge.cpp'), *objects, '-L', str(openssl / 'lib'), '-lcrypto',
        '-o', str(library)], check=True)
    return library


def prepare(out, verified_root):
    spec = importlib.util.spec_from_file_location('ufs_fixture_manifest', verified_root / 'tools/manifest.py')
    signer = importlib.util.module_from_spec(spec); spec.loader.exec_module(signer)
    keys = out / 'fixture-keys'; keys.mkdir(mode=0o700, exist_ok=True); os.chmod(keys,0o700)
    private, public = keys / 'owner.pem', keys / 'owner-public.pem'
    if not private.exists():
        fd = os.open(private,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600); os.close(fd)
        subprocess.run(['openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:3072',
            '-out',str(private)],check=True,capture_output=True)
    os.chmod(private,0o600)
    subprocess.run(['openssl','pkey','-in',str(private),'-pubout','-out',str(public)],check=True,capture_output=True)
    os_data = bytes((i*29+i//4096)&255 for i in range(33*4096+512))
    system_data = bytes((i*13+i//4096)&255 for i in range(4096+512))
    corrupt = bytearray(os_data); corrupt[33*4096] ^= 1
    swapped = os_data[4096:8192] + os_data[:4096] + os_data[8192:]
    windows = [('OS',os_data),('SystemOS',system_data),('Corrupt',bytes(corrupt)),('Swapped',swapped)]
    header = bytearray(4096); header[:8]=b'SHNGROOT'; struct.pack_into('<II',header,8,1,len(windows))
    cursor=4096; disk=bytearray(2*1024*1024); expected=[]
    for index,(name,data) in enumerate(windows):
        struct.pack_into('<32sQQII32s',header,24+index*128,name.encode(),cursor,len(data),512,1,hashlib.sha256(data).digest())
        disk[8*4096+cursor:8*4096+cursor+len(data)]=data
        expected.append((cursor,len(data)));cursor+=(len(data)+4095)&~4095
    struct.pack_into('<Q',header,16,cursor);header[4064:]=hashlib.sha256(header[:4064]).digest()
    disk[8*4096:9*4096]=header
    entries=bytearray(4096);entries[:16]=uuid.UUID('dd59ac14-13f5-4d77-801d-2e20bf564099').bytes_le
    entries[16:32]=uuid.UUID('d6ad4e45-9ddb-4cce-9827-4c3fd6b9da5a').bytes_le
    struct.pack_into('<QQ',entries,32,8,506);entries[56:74]='userdata\0'.encode('utf-16-le')
    gpt=bytearray(4096);gpt[:8]=b'EFI PART';struct.pack_into('<IIIIQQQQ',gpt,8,0x10000,92,0,0,1,511,6,506)
    gpt[56:72]=uuid.UUID('f8a8d015-0f98-4800-9c74-b023ca44099f').bytes_le
    struct.pack_into('<QIII',gpt,72,2,32,128,zlib.crc32(entries));struct.pack_into('<I',gpt,16,zlib.crc32(gpt[:92]))
    disk[4096:8192]=gpt;disk[8192:12288]=entries
    # Separate raw transport payload, outside the synthetic windows.
    pattern=bytes((i*29+i//4096)&255 for i in range(131072));disk[384*4096:416*4096]=pattern
    (out/'disk.raw').write_bytes(disk)
    policies=[]
    for name,data in windows[:2]:
        directory=out/('signed-'+name)
        signer.make(io.BytesIO(data),len(data),private,public,
            uuid.UUID('01e286f1-f4dd-4b2f-99d9-4973337fc788' if name=='OS' else 'db25b52a-1d3e-4bfc-b54c-360a158be156'),7,directory)
        policies.append((directory/'manifest.bin').read_bytes())
    return dict(pattern=pattern,windows=expected,os=os_data,system=system_data,policies=policies,
                key=(out/'signed-OS/trusted-key.bin').read_bytes())


def check(lib, fixture):
    lib.ufs_path_layout.restype=C.c_uint32
    lib.ufs_path_window.argtypes=[C.c_uint,C.c_uint];lib.ufs_path_window.restype=C.c_uint64
    lib.ufs_path_open.argtypes=[C.c_uint,C.c_void_p,C.c_size_t,C.c_void_p];lib.ufs_path_open.restype=C.c_uint32
    lib.ufs_path_read.argtypes=[C.c_uint64,C.c_uint64,C.c_void_p,C.c_uint64,C.POINTER(C.c_uint64)]
    lib.ufs_path_read.restype=C.c_uint32
    assert lib.ufs_path_layout()==0, 'Production GPT/manifest parser failed'
    for i,(offset,size) in enumerate(fixture['windows']):
        assert lib.ufs_path_window(i,0)==offset and lib.ufs_path_window(i,1)==size
    def opened(index,policy=0,envelope=None,key=None,expected=0):
        blob=fixture['policies'][policy] if envelope is None else envelope
        assert lib.ufs_path_open(index,blob,len(blob),fixture['key'] if key is None else key)==expected
    def read(block,count,expected,done,status=0):
        buf=(C.c_uint8*(count*512))(*([0xa5]*(count*512)));n=C.c_uint64()
        assert lib.ufs_path_read(block,count,buf,len(buf),C.byref(n))==status
        assert n.value==done and bytes(buf)==expected+bytes([0xa5])*(len(buf)-len(expected))
    opened(0)
    for offset,count in [(0,265),(1,257),(7,250),(263,2),(264,1)]:
        wanted=fixture['os'][offset*512:(offset+count)*512];read(offset,count,wanted,len(wanted))
    opened(1,policy=1);read(7,2,fixture['system'][7*512:],1024)
    # Corrupt final 512-byte block: previous verified 128K may be delivered;
    # no byte from the failing second chunk may leave the driver.
    opened(2);read(0,265,fixture['os'][:131072],131072,0xe00002c1)
    read(264,1,b'',0,0xe00002c1)
    opened(3);read(0,16,b'',0,0xe00002c1)  # valid blocks in wrong order
    blob=bytearray(fixture['policies'][0]);blob[160]^=1
    opened(0,envelope=bytes(blob),expected=3);read(0,1,b'',0,0xe00002c2)
    blob=bytearray(fixture['policies'][0]);blob[544]^=1
    opened(0,envelope=bytes(blob),expected=4)
    key=bytearray(fixture['key']);key[8]^=1
    opened(0,key=bytes(key),expected=2)
    opened(1,policy=0,expected=0x101)  # authentic policy for a different capacity
    opened(0,envelope=b'',expected=1)
    return {'gpt_manifest':'pass','rsa3072_owner_policy':'pass','window_512_4k_128k_tail':'pass',
            'late_corruption_partial_progress':'pass','swapped_blocks_no_copy':'pass',
            'bad_signature_hash_table_anchor_capacity_unsigned':'pass'}
