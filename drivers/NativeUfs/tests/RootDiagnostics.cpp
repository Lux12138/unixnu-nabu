// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include "../RootDiagnostics.h"

int main(int argc, char **)
{
  UfsRootDiagnostics d;
  d.stage = "NOP OUT";
  d.dma = 0x8ca774000ULL;
  d.list = 0x8ca774000ULL;
  d.result = 0xe00002d6;
  d.ocs = 15;
  d.smr = 0x80000060;
  d.s2cr = 127;
  d.cbar = 0x10000;
  d.command = 1;
  d.far = 0x8cb5e9000ULL;
  d.contiguous = 139264;
  d.polls = 100001;
  d.uic[4] = 0x80000001;
  d.protocol = 6;
  d.residual = 4096;
  d.sense = 0x062900;
  d.publicationPhase = 5;
  d.publicationWindow = 1;
  char full[2500];
  int length = formatUfsRootDiagnostics(full, sizeof(full), d, "transport timeout fixture");
  assert(length > 0 && length < 2000 && size_t(length) == strlen(full));
  assert(strstr(full, "DMA=8ca774000 UTRL=8ca774000"));
  assert(strstr(full, "result=e00002d6"));
  assert(strstr(full, "ocs=0f"));
  assert(strstr(full, "verify=ffffffff")); // Not sampled must not look successful.
  assert(strstr(full, "far=8cb5e9000"));
  assert(strstr(full, "polls=100001"));
  assert(strstr(full, "dme=80000001"));
  assert(strstr(full, "reject=6 residual=4096 sense=062900"));
  assert(strstr(full, "pubPhase=5 window=1"));
  char passive[2500];
  formatUfsRootDiagnostics(passive,sizeof(passive),d,"cached values; not a new HW fault sample","UFS CACHED SNAPSHOT");
  assert(strstr(passive,"UFS CACHED SNAPSHOT:") == passive && !strstr(passive,"UFS ROOT FAILURE"));
  assert(strstr(passive,"far=8cb5e9000"));
  // Every truncation boundary, including zero and a one-byte destination.
  for (size_t size = 0; size <= sizeof(full); ++size) {
    unsigned char storage[2502];
    memset(storage, 0xa5, sizeof(storage));
    int result = formatUfsRootDiagnostics(reinterpret_cast<char *>(storage + 1), size, d,
                                          "transport timeout fixture");
    assert(result == length && storage[0] == 0xa5 && storage[size + 1] == 0xa5);
    if (size) assert(storage[1 + (size <= size_t(length) ? size - 1 : size_t(length))] == 0);
  }
  if (argc > 1) {
    // Maximum-width numeric fields plus all eight bounded media entries.
    // The stage pointer is the only noninteger field; preserve it.
    memset(reinterpret_cast<uint8_t *>(&d) + offsetof(UfsRootDiagnostics,dma), 0xff,
           sizeof(d) - offsetof(UfsRootDiagnostics,dma));
    d.stage = "media publication";
    int used = formatUfsRootDiagnostics(full,sizeof(full),d,"SystemOS signed volume not found");
    used += snprintf(full+used,sizeof(full)-used,"Waiting for signed UUID FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF\n");
    for (unsigned i=0;i<8;++i) {
      assert(used < int(sizeof(full))-110);
      used += snprintf(full+used,sizeof(full)-used,
        "media abcdefghijklmnopqrstuvwx bytes=ffffffffffffffff uuid=FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF\n");
    }
    assert(used < int(sizeof(full)) && size_t(used)==strlen(full));
  }
  fputs(full, stdout);
}
