#pragma once
// maxmesh advert blob store (ESP32 + LittleFS).
//
// MeshCore keeps the last advert of every contact ("blob", ~110-180 bytes) as one file per contact in /bl/.
// On LittleFS a folder with hundreds of files makes every file operation take 1-3 s, and rewriting a record
// in the middle of one big file is just as slow (LittleFS copies everything after it). So adverts are kept in
// 32 small shard files (/ab_00 .. /ab_31), chosen by the first key byte, each at most 16 records (~3.3 KB,
// less than one flash block): saving an advert rewrites one small file (tens of ms). A copy lives in PSRAM.
#include <FS.h>
#include <stdint.h>
#include <sys/stat.h>

// quiet existence check (Arduino's exists() logs an error for every missing file)
static inline bool mxExists(const char* path) {
  char p[80]; snprintf(p, sizeof(p), "/littlefs%s", path);
  struct stat st; return stat(p, &st) == 0;
}

#define MXB_SHARDS 32
#define MXB_PER    16
#define MXB_RECS   (MXB_SHARDS * MXB_PER)   // 512 > MAX_CONTACTS
#define MXB_DATA   192                      // largest advert seen ~176 bytes
#define MXB_LEGACY_FILE "/adv_blobs2"       // single-file layout from a test build (deleted)

struct MxBlobRec {
  uint8_t key[8];             // first 8 bytes of the public key
  uint8_t len;                // 0 = empty slot
  uint8_t pad[3];
  uint32_t ts;                // when stored (for evicting the oldest)
  uint8_t data[MXB_DATA];
};

static inline int mxbShard(const uint8_t* key) { return key[0] % MXB_SHARDS; }
static inline void mxbShardPath(int s, char* out) { sprintf(out, "/ab_%02d", s); }

// Write one shard (only its used records) from the PSRAM table.
static inline bool mxbWriteShard(fs::FS& fs, const MxBlobRec* all, int s) {
  char p[12]; mxbShardPath(s, p);
  const MxBlobRec* r = all + s * MXB_PER;
  int used = 0;
  for (int i = 0; i < MXB_PER; i++) if (r[i].len) used++;
  if (!used) { fs.remove(p); return true; }
  File f = fs.open(p, "w", true);
  if (!f) return false;
  bool ok = true;
  for (int i = 0; i < MXB_PER && ok; i++) if (r[i].len) ok = f.write((const uint8_t*)&r[i], sizeof(MxBlobRec)) == sizeof(MxBlobRec);
  f.close();
  return ok;
}

// Read every shard into a zeroed table of MXB_RECS records.
static inline void mxbReadAll(fs::FS& fs, MxBlobRec* all) {
  for (int s = 0; s < MXB_SHARDS; s++) {
    char p[12]; mxbShardPath(s, p);
    if (!mxExists(p)) continue;
    File f = fs.open(p, "r");
    if (!f) continue;
    MxBlobRec* r = all + s * MXB_PER;
    for (int i = 0; i < MXB_PER; i++) if (f.read((uint8_t*)&r[i], sizeof(MxBlobRec)) != sizeof(MxBlobRec)) { memset(&r[i], 0, sizeof(MxBlobRec)); break; }
    f.close();
  }
}

// Put a record into its shard's slots in the table (empty slot, else the oldest). Returns the index.
static inline int mxbPlace(MxBlobRec* all, const uint8_t* key, int key_len, int* found = nullptr) {
  int s = mxbShard(key), n = key_len < 8 ? key_len : 8;
  MxBlobRec* r = all + s * MXB_PER;
  for (int i = 0; i < MXB_PER; i++) if (r[i].len && memcmp(r[i].key, key, n) == 0) { if (found) *found = 1; return s * MXB_PER + i; }
  if (found) *found = 0;
  int best = 0; uint32_t oldest = 0xFFFFFFFF;
  for (int i = 0; i < MXB_PER; i++) {
    if (!r[i].len) return s * MXB_PER + i;
    if (r[i].ts < oldest) { oldest = r[i].ts; best = i; }
  }
  return s * MXB_PER + best;
}
