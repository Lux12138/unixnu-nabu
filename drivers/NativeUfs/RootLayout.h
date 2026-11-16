// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Shared production parser. Requires hostRead4k, SHA256, diagnostics and C memory primitives.
// ---- LUN 0 GPT and the root-image manifest -------------------------------------------------
struct RootWindow {
  char name[32];
  uint64_t offset;   // bytes from the start of the userdata partition
  uint64_t size;     // bytes
  uint8_t sha256[32];
};

struct RootLayout {
  uint64_t userdataFirst, userdataLast;   // 4096-byte LBAs on LUN 0
  uint64_t miscFirst;                     // misc (bootloader_message) first LBA, 0 when absent
  bool miscFound;
  uint64_t imageBytes;                    // size of the flashed root image from the manifest (0 when none)
  uint64_t gptEntriesLba;                 // GPT facts recorded for the persistent store's before/after comparison
  uint32_t gptHeaderCrc, gptEntriesCrc, gptEntryCount;
  uint32_t windows;
  RootWindow window[4];
  uint32_t manifestStatus;                // 0 ok, 1 no manifest magic, 2 bad version/count, 3 hash, 4 bounds, 5 names, 6 overlap, 7 attributes
  uint32_t magic0, magic1;                // first 8 bytes of the partition's first block
  uint32_t ext4Magic;                     // 16 bits at byte 1080 of the partition (0xef53 on Linux ext4)
  uint32_t ext4Blocks;                    // s_blocks_count_lo at byte 1028
};

static uint8_t gScratch[kDataMax];

static uint32_t le32 (const uint8_t *at)
{
  return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
}

static uint64_t le64 (const uint8_t *at)
{
  return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
}

// GPT uses the reflected IEEE CRC32 polynomial. The header CRC field is zero
// during its own checksum; array CRC covers exactly count * entrySize bytes.
static uint32_t rootCrc32 (const uint8_t *data, size_t bytes, bool header = false)
{
  uint32_t crc = 0xffffffffU;
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= header && i >= 16 && i < 20 ? 0 : data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
  }
  return ~crc;
}

static bool rootEntryUsed (const uint8_t *entry)
{
  uint8_t used = 0;
  for (unsigned i = 0; i < 16; ++i) used |= entry[i];
  return used != 0;
}

// Find the partition named "userdata" in LUN 0's primary GPT (4096-byte sectors), then read the
// manifest at its first block. Read-only. Returns false only if the GPT itself is unusable.
static bool findRootLayout (RootLayout *layout, const char **why)
{
  gRootDiagnostic.stage = "GPT header";
  bzero (layout, sizeof (*layout));
  if (hostRead4k (0, 1, 1, gScratch) != kIOReturnSuccess) {
    *why = "GPT header read";
    return false;
  }
  if (le32 (gScratch) != 0x20494645U || le32 (gScratch + 4) != 0x54524150U || le32 (gScratch + 84) != 128) {
    *why = "GPT header";
    return false;
  }
  uint32_t headerBytes = le32 (gScratch + 12);
  if (le32 (gScratch + 8) != 0x00010000 || headerBytes < 92 || headerBytes > 4096 ||
      le32 (gScratch + 20) || le64 (gScratch + 24) != 1 ||
      rootCrc32 (gScratch, headerBytes, true) != le32 (gScratch + 16)) {
    *why = "GPT header CRC/revision";
    return false;
  }
  uint64_t backup = le64 (gScratch + 32), first = le64 (gScratch + 40), last = le64 (gScratch + 48);
  uint32_t entriesCrc = le32 (gScratch + 88);
  uint64_t entriesLba = le64 (gScratch + 72);
  uint32_t count = le32 (gScratch + 80);
  layout->gptHeaderCrc = le32 (gScratch + 16);
  layout->gptEntriesCrc = entriesCrc;
  layout->gptEntriesLba = entriesLba;
  layout->gptEntryCount = count;
  if (entriesLba < 2 || entriesLba > 64 || !count || count > 128) {
    *why = "GPT geometry";
    return false;
  }
  uint32_t blocks = (count * 128 + 4095) / 4096;
  // This path uses READ(10), so reject geometry beyond its actual LBA range.
  if (first < entriesLba + blocks || last < first || backup <= last || backup > UINT32_MAX ||
      backup - last <= blocks) {
    *why = "GPT usable range";
    return false;
  }
  gRootDiagnostic.stage = "GPT entries";
  if (hostRead4k (0, entriesLba, blocks, gScratch) != kIOReturnSuccess) {
    *why = "GPT entries read";
    return false;
  }
  if (rootCrc32 (gScratch, count * 128) != entriesCrc) {
    *why = "GPT entries CRC";
    return false;
  }
  static const char wanted[] = "userdata";
  static const char wantedMisc[] = "misc";
  bool found = false;
  for (uint32_t index = 0; index < count; index++) {
    const uint8_t *entry = gScratch + index * 128;
    if (!rootEntryUsed (entry)) continue;
    uint64_t entryFirst = le64 (entry + 32), entryLast = le64 (entry + 40);
    if (entryFirst < first || entryLast < entryFirst || entryLast > last) {
      *why = "GPT partition bounds";
      return false;
    }
    for (uint32_t prior = 0; prior < index; ++prior) {
      const uint8_t *other = gScratch + prior * 128;
      if (rootEntryUsed (other) && entryFirst <= le64 (other + 40) && le64 (other + 32) <= entryLast) {
        *why = "GPT partition overlap";
        return false;
      }
    }
    bool match = true;
    for (unsigned c = 0; c < sizeof (wanted); c++) {
      if (entry[56 + 2 * c] != static_cast<uint8_t> (wanted[c]) || entry[57 + 2 * c] != 0) {
        match = false;
        break;
      }
    }
    if (match) {
      if (found) {
        *why = "duplicate userdata partition";
        return false;
      }
      layout->userdataFirst = le64 (entry + 32);
      layout->userdataLast = le64 (entry + 40);
      found = true;
    }
    bool matchMisc = true;
    for (unsigned c = 0; c < sizeof (wantedMisc); c++) {
      if (entry[56 + 2 * c] != static_cast<uint8_t> (wantedMisc[c]) || entry[57 + 2 * c] != 0) {
        matchMisc = false;
        break;
      }
    }
    if (matchMisc) {
      layout->miscFirst = entryFirst;
      layout->miscFound = true;
    }
  }
  if (!found || layout->userdataLast < layout->userdataFirst) {
    *why = "no userdata partition";
    return false;
  }
  gRootDiagnostic.first = layout->userdataFirst;
  gRootDiagnostic.last = layout->userdataLast;
  gRootDiagnostic.stage = "root manifest";
  // First block of the partition: manifest if the root image was flashed, ext4 otherwise.
  if (hostRead4k (0, layout->userdataFirst, 1, gScratch) != kIOReturnSuccess) {
    *why = "userdata first block read";
    return false;
  }
  layout->magic0 = le32 (gScratch);
  layout->magic1 = le32 (gScratch + 4);
  layout->ext4Magic = uint32_t (gScratch[1080]) | (uint32_t (gScratch[1081]) << 8);
  layout->ext4Blocks = le32 (gScratch + 1028);
  layout->manifestStatus = 1;
  if (layout->magic0 == 0x474e4853U && layout->magic1 == 0x544f4f52U) {        // "SHNGROOT"
    uint32_t version = le32 (gScratch + 8), n = le32 (gScratch + 12);
    uint64_t imageBytes = le64 (gScratch + 16);
    layout->manifestStatus = 2;
    if (version == 1 && n && n <= 4) {
      SHA256_CTX context;
      uint8_t digest[32];
      SHA256_Init (&context);
      SHA256_Update (&context, gScratch, 4064);
      SHA256_Final (digest, &context);
      layout->manifestStatus = 3;
      if (memcmp (digest, gScratch + 4064, 32) == 0) {
        uint64_t partitionBytes = (layout->userdataLast - layout->userdataFirst + 1) * 4096ULL;
        layout->manifestStatus = 4;
        bool bounds = imageBytes >= 4096 && !(imageBytes & 4095) && imageBytes <= partitionBytes;
        for (uint32_t index = 0; index < n && bounds; index++) {
          const uint8_t *entry = gScratch + 24 + 128 * index;
          uint64_t offset = le64 (entry + 32), size = le64 (entry + 40);
          if ((offset & 4095) || offset < 4096 || (size & 511) || !size || offset >= imageBytes ||
              size > imageBytes - offset || ((size + 4095) & ~4095ULL) > imageBytes - offset) {
            bounds = false;
            break;
          }
          if (le32 (entry + 48) != 512 || le32 (entry + 52) != 1) {
            layout->manifestStatus = 7; bounds = false; break;
          }
          unsigned length = 0;
          while (length < 32 && entry[length] >= 0x20 && entry[length] <= 0x7e) ++length;
          if (!length || length == 32 || entry[length] != 0) {
            layout->manifestStatus = 5; bounds = false; break;
          }
          for (uint32_t prior = 0; prior < index; ++prior) {
            const RootWindow &other = layout->window[prior];
            if (!strcmp (reinterpret_cast<const char *> (entry), other.name)) {
              layout->manifestStatus = 5; bounds = false; break;
            }
            // Include the final sector padding that a physical 4K read fetches.
            if (offset < other.offset + ((other.size + 4095) & ~4095ULL) &&
                other.offset < offset + ((size + 4095) & ~4095ULL)) {
              layout->manifestStatus = 6; bounds = false; break;
            }
          }
          if (!bounds) break;
          memcpy (layout->window[index].name, entry, 32);
          layout->window[index].name[31] = 0;
          layout->window[index].offset = offset;
          layout->window[index].size = size;
          memcpy (layout->window[index].sha256, entry + 56, 32);
        }
        if (bounds) {
          layout->windows = n;
          layout->imageBytes = imageBytes;
          layout->manifestStatus = 0;
        }
      }
    }
  }
  gRootDiagnostic.magic0 = layout->magic0;
  gRootDiagnostic.magic1 = layout->magic1;
  gRootDiagnostic.manifest = layout->manifestStatus;
  gRootDiagnostic.windows = layout->windows;
  return true;
}
