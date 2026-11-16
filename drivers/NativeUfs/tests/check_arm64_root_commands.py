#!/usr/bin/env python3
"""Bounded EL1 TCG execution of production commands and cache/MMIO primitives.

Only a synthetic disk and QEMU virt addresses. No device access. QEMU does
not model SM8550 cache coherency, its SMMU or its bus/power/timing behavior.
Generated files and raw logs belong in an ignored output directory.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def extract(text, name):
    matches=list(re.finditer(r'^static (?:uint32_t|void) '+name+r' \(',text,re.M))
    assert len(matches)==1, f'ambiguous/missing production function {name}'
    start=matches[0].start(); brace=text.index('{',start); depth=1; end=brace+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}'); end+=1
    return text[start:end]+'\n'


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--qemu',default='qemu-system-aarch64')
    parser.add_argument('--clang',default='clang++')
    parser.add_argument('--linker',default='ld.lld')
    args=parser.parse_args()
    out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
    if shutil.disk_usage(out).free < 5*1024**3+512*1024**2:
        raise SystemExit('Keep 5 GiB free plus fixture headroom')
    source=Path(__file__).resolve().parent
    production=source.parent/'NativeUfs.cpp'
    extracted='\n'.join(extract(production.read_text(),name) for name in
                        ('read32','write32','cacheCivac','cacheInvalidate','hostWrite'))
    (out/'RootHardwareExtracted.h').write_text(extracted)
    (out/'link.ld').write_text('''ENTRY(_start)
SECTIONS {
 . = 0x40200000;
 .text : { *(.text.entry) *(.text*) }
 .rodata : { *(.rodata*) }
 .data : { *(.data*) }
 . = ALIGN(4096);
 __bss_start = .;
 .bss : { *(.bss*) *(COMMON) }
 . = ALIGN(8); __bss_end = .;
 ASSERT(. < 0x40400000, "fixture exceeds identity mapping budget")
}
''')
    (out/'blank-firmware.bin').write_bytes(bytes(4096))
    disk=bytearray(2*1024*1024)
    disk[8*4096:8*4096+131072]=bytes(((i*17+(i>>8)*31)^0xa5)&255 for i in range(131072))
    (out/'disk.raw').write_bytes(disk)
    compiler=shutil.which(args.clang); linker=shutil.which(args.linker); qemu=shutil.which(args.qemu)
    assert compiler and linker and qemu,'Missing cross compiler, linker or QEMU'
    common=[compiler,'--target=aarch64-none-elf','-O2','-ffreestanding','-fno-builtin','-nostdinc++',
            '-fno-exceptions','-fno-rtti','-fno-stack-protector','-mgeneral-regs-only',
            '-fno-strict-aliasing','-Wall','-Wextra','-Werror','-Wno-unused-function',
            '-I',str(out)]
    subprocess.run([compiler,'--target=aarch64-none-elf','-c',str(source/'RootCommandsArm64.S'),
                    '-o',str(out/'entry.o')],check=True)
    results=[]
    for name,physical,bad_dma,sharing in [('low',0x40010000,0,2),('high',0x8cb5e9000,0,2),
                                        ('high-inner-shareable',0x8cb5e9000,0,3),
                                        ('truncated-high',0x8cb5e9000,1,2),('virtual-as-physical',0x8cb5e9000,2,2)]:
        directory=out/name; directory.mkdir(exist_ok=True)
        compile_command=common+[f'-DTEST_DMA_PHYS={physical:#x}ULL']
        compile_command+=[f'-DTEST_BAD_DMA={bad_dma}',f'-DTEST_SHAREABILITY={sharing}']
        compile_command+=['-c',str(source/'RootCommandsArm64.cpp'),'-o',str(directory/'guest.o')]
        subprocess.run(compile_command,check=True)
        subprocess.run([linker,'-T',str(out/'link.ld'),str(out/'entry.o'),str(directory/'guest.o'),
                        '-o',str(directory/'guest.elf')],check=True)
        command=[qemu,'-machine','virt,gic-version=3,highmem-ecam=off,acpi=on,virtualization=off,secure=off',
                 '-cpu','cortex-a710','-accel','tcg','-m','128M,slots=1,maxmem=64G',
                 '-bios',str(out/'blank-firmware.bin'),
                 '-object','memory-backend-ram,id=hi,size=256M',
                 '-device','pc-dimm,memdev=hi,addr=0x8c0000000',
                 '-device','ufs,id=ufs0,bus=pcie.0,addr=1.0',
                 '-drive',f'if=none,id=disk,format=raw,readonly=on,file={out / "disk.raw"}',
                 '-device','ufs-lu,bus=ufs0,drive=disk,lun=0',
                 '-device',f'loader,file={directory / "guest.elf"},cpu-num=0',
                 '-nodefaults','-display','none','-serial','stdio','-monitor','none','-no-reboot',
                 '-semihosting-config','enable=on,target=native']
        (directory/'commands.json').write_text(json.dumps({'compile':compile_command,'qemu':command},indent=2)+'\n')
        try:
            result=subprocess.run(command,capture_output=True,timeout=15)
            log=result.stdout.decode(errors='replace'); errors=result.stderr.decode(errors='replace')
        except subprocess.TimeoutExpired as error:
            (directory/'serial.log').write_bytes(error.stdout or b'')
            (directory/'qemu.log').write_bytes(error.stderr or b'')
            raise
        (directory/'serial.log').write_text(log); (directory/'qemu.log').write_text(errors)
        print(name+'\n'+log,flush=True)
        assert result.returncode==0 and 'FAIL' not in log,(name,result.returncode,errors)
        assert ('PASS rejected bad DMA without resubmission' if bad_dma else 'PASS ALL') in log,name
        results.append({'case':name,'dma_pa':hex(physical),'shareability':sharing,'passed':True,
                        'registers':dict(re.findall(r'^(\w+)=(0x[0-9a-f]+)$',log,re.M))})
    files=[production,source/'RootCommandsArm64.cpp',source/'RootCommandsArm64.S',Path(__file__).resolve()]
    files += [source.parent/name for name in ('RootCommands.h','RootQueue.h','TransferPoll.h',
                                             'RootTransportLayout.h','RootDiagnostics.h')]
    report={'results':results,'qemu':subprocess.check_output([qemu,'--version'],text=True).splitlines()[0],
            'compiler':subprocess.check_output([compiler,'--version'],text=True).splitlines()[0],
            'source_revision':subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip(),
            'source_sha256':{str(f.relative_to(source.parent)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},
            'extracted_primitives_sha256':hashlib.sha256(extracted.encode()).hexdigest(),
            'scope':'EL1 TCG, active native 4K tables, cached VA != DMA PA, production ARM64 commands/DC/barriers; '
                    'not XNU allocation/locks, real cache coherency, SM8550 SMMU/bus/timing or hardware acceptance'}
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')


if __name__=='__main__':
    main()
