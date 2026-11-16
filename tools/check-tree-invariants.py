#!/usr/bin/env python3
"""Tree invariants that must hold before a kernel build, or the build silently produces a
different machine than the one the round is about.

Run from the assembled tree root (scripts/build-xnu-4k-baseline.sh calls it after the device
patch series is applied, before make). Non-zero exit on the first failure.

Why this exists: two of the changes that make the eight-core bring-up work lived only in the
assembled work tree, so `unixnu tree` (which regenerates that tree from the framework) dropped
them, and a build then produced a four-core kernel with an Affinity0-only reset-vector mask --
a board round that can only ever show "single core reaches the page". A third change, the
framework patch that guards the tunables include, was lost the same way when the work tree was
checked back out, and the build failed on a missing header instead. Both classes are invisible
to a source grep of the device repo, which is why they are checked here, on the tree.
"""
import io
import re
import sys

START = '.cache/xnu-work/osfmk/arm64/start.s'
BOARD = '.cache/xnu-work/pexpert/pexpert/arm64/board_config.h'


def read(path):
    try:
        return io.open(path, encoding='utf-8', errors='replace').read()
    except OSError as exc:
        print('MISSING %s: %s' % (path, exc))
        return None


def main():
    failures = []

    start = read(START)
    if start is None:
        failures.append(START)
    else:
        # 1. the reset-vector mask must cover Affinity0+Affinity1: SM8150 carries the cluster
        #    number in Affinity1, so an Affinity0-only mask can never match a CpDataEntries row.
        #    Checked as "the old mask is gone" because the HAS_CLUSTER branch upstream already
        #    carries #0xFFFF, so a presence test would pass on an unpatched file.
        if re.search(r'and\s+x0,\s*x15,\s*#0xFF(?![0-9A-Fa-f])', start):
            failures.append('%s: still `and x0, x15, #0xFF` (Affinity0-only mask)' % START)
        elif not re.search(r'and\s+x0,\s*x15,\s*#0xFFFF', start):
            failures.append('%s: no `and x0, x15, #0xFFFF` (Affinity0+Affinity1 mask)' % START)
        # 2. the walk window must span eight entries. In the source it is written as
        #    `MAX_CPUS * 16`; the disassembled kernel shows the folded immediate #0x80.
        if not re.search(r'add\s+x3,\s*x1,\s*MAX_CPUS \* 16', start):
            failures.append('%s: no `add x3, x1, MAX_CPUS * 16` (eight-entry walk window)'
                            % START)
        # 3. the tunables include must sit inside the APPLE_ARM64_ARCH_FAMILY guard the framework
        #    patch adds; unguarded, the build looks for arm64/tunables/tunables.s, which this
        #    target does not have.
        lines = start.splitlines()
        guarded = False
        depth = 0
        for line in lines:
            if re.match(r'\s*#if\s+defined\(APPLE_ARM64_ARCH_FAMILY\)', line):
                depth += 1
                continue
            if re.match(r'\s*#endif', line) and depth:
                depth -= 1
                continue
            if 'arm64/tunables/tunables.s' in line and depth == 0:
                failures.append('%s: tunables include outside the APPLE_ARM64_ARCH_FAMILY guard'
                                % START)
                break
        else:
            guarded = True
        if not guarded and not any('tunables include' in f for f in failures):
            failures.append('%s: tunables include not found at all' % START)

    board = read(BOARD)
    if board is None:
        failures.append(BOARD)
    else:
        # 4. BCM2837 must stay at eight slots and one cluster: HAS_CLUSTER has to stay off,
        #    because with it on the DT reg values 0x0..0x700 become eight clusters and trip
        #    assert(cpu->cluster_id < MAX_CPU_CLUSTERS).
        m = re.search(r'#ifdef ARM64_BOARD_CONFIG_BCM2837(.*?)#endif', board, re.S)
        if not m:
            failures.append('%s: no ARM64_BOARD_CONFIG_BCM2837 block' % BOARD)
        else:
            block = m.group(1)
            mc = re.search(r'#define\s+MAX_CPUS\s+(\d+)', block)
            cl = re.search(r'#define\s+MAX_CPU_CLUSTERS\s+(\d+)', block)
            if not mc or mc.group(1) != '8':
                failures.append('%s: BCM2837 MAX_CPUS is %s, expected 8'
                                % (BOARD, mc.group(1) if mc else 'absent'))
            if not cl or cl.group(1) != '1':
                failures.append('%s: BCM2837 MAX_CPU_CLUSTERS is %s, expected 1'
                                % (BOARD, cl.group(1) if cl else 'absent'))

    if failures:
        print('tree invariants FAILED:')
        for f in failures:
            print('  - %s' % f)
        return 1
    print('tree invariants OK: mask 0xFFFF, window 0x80, tunables guarded, BCM2837 8/1')
    return 0


if __name__ == '__main__':
    sys.exit(main())
