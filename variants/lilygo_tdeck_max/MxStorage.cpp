// maxmesh storage: switch the data partition from SPIFFS to LittleFS, keeping the user's data.
//
// SPIFFS on this 3.4 MB partition is very slow (opening a file ~2 s, writing 58 KB ~7 s), and MeshCore
// rewrites the contact list after every advert it hears, so the UI froze for ~11 s at a time. LittleFS is
// far faster and copy-on-write (a power cut never leaves a half-written file).
//
// Migration (first boot after updating from SPIFFS firmware):
//   1. read every SPIFFS file into PSRAM (a few hundred KB)
//   2. write that bundle to the unused second app partition ("app1", 6.4 MB; maxmesh doesn't use OTA),
//      header written last, then read back and CRC-checked   -> a power cut from here on can be resumed
//   3. format the partition as LittleFS and write the files back, each verified by CRC
//   4. write /.mx_migrated (bundle CRC), then erase the bundle header
// On every boot: if a valid bundle header exists and /.mx_migrated doesn't match it, step 3 is redone.
#include "MxStorage.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <SPIFFS.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <esp_rom_crc.h>
#include <dirent.h>
#include <sys/stat.h>
#include <helpers/ui/DisplayDriver.h>
#include <vector>
#include "MxBlobStore.h"

#define MIG_MAGIC   0x4D584D47u   // "MXMG"
#define MIG_VERSION 1
#define MIG_MARKER  "/.mx_migrated"

struct MigHdr { uint32_t magic, version, len, crc, nfiles; };

static int s_result = -1, s_files = 0, s_bytes = 0;

int mxStorageMigration(int* files, int* bytes) {
  if (files) *files = s_files;
  if (bytes) *bytes = s_bytes;
  return s_result;
}

static void show(DisplayDriver* disp, const char* l1, const char* l2) {
  Serial.printf("storage: %s %s\n", l1, l2 ? l2 : ""); Serial.flush();
  if (!disp) return;
  disp->startFrame();
  disp->drawTextCentered(disp->width() / 2, 120, l1);
  if (l2) disp->drawTextCentered(disp->width() / 2, 145, l2);
  disp->endFrame();
  Serial.printf("storage: screen updated (%u ms)\n", (unsigned)millis()); Serial.flush();
}

static const esp_partition_t* scratchPart() {
  const esp_partition_t* p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
  if (!p || p == esp_ota_get_running_partition()) return nullptr;   // never touch the running firmware
  return p;
}

static bool readHdr(const esp_partition_t* p, MigHdr& h) {
  if (!p || esp_partition_read(p, 0, &h, sizeof(h)) != ESP_OK) return false;
  return h.magic == MIG_MAGIC && h.version == MIG_VERSION && h.len > 0 &&
         h.len <= p->size - sizeof(MigHdr) && h.nfiles > 0 && h.nfiles < 10000;
}

static void clearScratch(const esp_partition_t* p, uint32_t len = 0) {
  // erase the header and (if known) the whole copy, so no second copy of the user's data stays in app1
  if (p) esp_partition_erase_range(p, 0, (len + sizeof(MigHdr) + 4095) & ~4095u);
}

// ---- 1. collect SPIFFS files: payload = { u16 name_len, name, u32 size, data }* ----
static uint8_t* collectSpiffs(uint32_t& len, uint32_t& nfiles) {
  len = 0; nfiles = 0;
  DIR* d = opendir("/spiffs");
  if (!d) return nullptr;
  struct dirent* e;
  while ((e = readdir(d)) != nullptr) {
    char path[300]; snprintf(path, sizeof(path), "/spiffs/%s", e->d_name);
    struct stat st; if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
    len += 2 + strlen(e->d_name) + 4 + st.st_size; nfiles++;
  }
  closedir(d);
  if (!nfiles) return nullptr;
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) return nullptr;
  uint32_t pos = 0, n = 0;
  d = opendir("/spiffs");
  while (d && (e = readdir(d)) != nullptr) {
    char path[300]; snprintf(path, sizeof(path), "/spiffs/%s", e->d_name);
    struct stat st; if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) continue;
    uint16_t nl = strlen(e->d_name); uint32_t sz = st.st_size;
    if (pos + 2 + nl + 4 + sz > len) { closedir(d); free(buf); return nullptr; }
    memcpy(buf + pos, &nl, 2); pos += 2;
    memcpy(buf + pos, e->d_name, nl); pos += nl;
    memcpy(buf + pos, &sz, 4); pos += 4;
    FILE* f = fopen(path, "rb");
    if (!f || fread(buf + pos, 1, sz, f) != sz) { if (f) fclose(f); closedir(d); free(buf); return nullptr; }
    fclose(f);
    pos += sz; n++;
    Serial.printf("storage:   read %s (%u bytes)\n", e->d_name, (unsigned)sz);
  }
  if (d) closedir(d);
  if (pos != len || n != nfiles) { free(buf); return nullptr; }
  return buf;
}

// ---- 2. bundle <-> scratch partition ----
static bool saveScratch(const esp_partition_t* p, const uint8_t* buf, uint32_t len, uint32_t nfiles, uint32_t crc) {
  if (!p || len + sizeof(MigHdr) > p->size) return false;
  uint32_t span = (len + sizeof(MigHdr) + 4095) & ~4095u;
  if (esp_partition_erase_range(p, 0, span) != ESP_OK) return false;
  if (esp_partition_write(p, sizeof(MigHdr), buf, len) != ESP_OK) return false;
  // verify the payload before making it valid
  uint8_t chunk[1024]; uint32_t c = 0;
  for (uint32_t off = 0; off < len; off += sizeof(chunk)) {
    uint32_t n = min((uint32_t)sizeof(chunk), len - off);
    if (esp_partition_read(p, sizeof(MigHdr) + off, chunk, n) != ESP_OK) return false;
    c = esp_rom_crc32_le(c, chunk, n);
  }
  if (c != crc) return false;
  MigHdr h = { MIG_MAGIC, MIG_VERSION, len, crc, nfiles };
  return esp_partition_write(p, 0, &h, sizeof(h)) == ESP_OK;     // header last
}

static uint8_t* loadScratch(const esp_partition_t* p, const MigHdr& h) {
  uint8_t* buf = (uint8_t*)ps_malloc(h.len);
  if (!buf) return nullptr;
  if (esp_partition_read(p, sizeof(MigHdr), buf, h.len) != ESP_OK || esp_rom_crc32_le(0, buf, h.len) != h.crc) {
    free(buf); return nullptr;
  }
  return buf;
}

static bool markerMatches(uint32_t crc) {
  if (!mxExists(MIG_MARKER)) return false;
  File f = LittleFS.open(MIG_MARKER, "r");
  uint32_t v = 0; bool ok = f && f.read((uint8_t*)&v, 4) == 4 && v == crc;
  if (f) f.close();
  return ok;
}

// ---- 3. write the bundle's files into LittleFS, verifying each ----
static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static bool restore(const uint8_t* buf, uint32_t len, uint32_t nfiles, uint32_t crc) {
  uint32_t pos = 0, n = 0;
  s_files = 0; s_bytes = 0;
  MxBlobRec* blobs = (MxBlobRec*)ps_calloc(MXB_RECS, sizeof(MxBlobRec));
  if (!blobs) return false;
  int nb = 0;
  while (pos < len) {
    uint16_t nl; uint32_t sz;
    if (pos + 2 > len) return false;
    memcpy(&nl, buf + pos, 2); pos += 2;
    if (nl == 0 || nl > 250 || pos + nl + 4 > len) return false;
    char path[260]; path[0] = '/'; memcpy(path + 1, buf + pos, nl); path[nl + 1] = 0; pos += nl;

    memcpy(&sz, buf + pos, 4); pos += 4;
    if (pos + sz > len) return false;
    const uint8_t* data = buf + pos; pos += sz;
    if (strncmp(path, "/bl/", 4) == 0) {           // advert blob file -> record in the single blob file
      const char* hx = path + 4;
      uint8_t key[8]; bool ok = strlen(hx) == 16 && sz > 0 && sz <= MXB_DATA;
      for (int k = 0; k < 8 && ok; k++) { int a = hexv(hx[2 * k]), b = hexv(hx[2 * k + 1]); if (a < 0 || b < 0) ok = false; else key[k] = (a << 4) | b; }
      if (ok) {
        MxBlobRec& r = blobs[mxbPlace(blobs, key, 8)];
        memset(&r, 0, sizeof(r)); memcpy(r.key, key, 8); r.len = sz; r.ts = 1; memcpy(r.data, data, sz); nb++;
      }
      n++; s_files++; s_bytes += sz;
      continue;
    }
    bool trace = n < 3;
    if (trace) { Serial.printf("storage:   [%u] open %s\n", (unsigned)n, path); Serial.flush(); }
    File f = LittleFS.open(path, "w", true);                 // creates parent folders (e.g. /bl/...)
    if (trace) { Serial.printf("storage:   [%u] opened=%d\n", (unsigned)n, (int)(bool)f); Serial.flush(); }
    if (!f) { Serial.printf("storage:   can't create %s\n", path); return false; }
    size_t w = sz ? f.write(data, sz) : 0; f.close();
    if (trace) { Serial.printf("storage:   [%u] wrote %u/%u\n", (unsigned)n, (unsigned)w, (unsigned)sz); Serial.flush(); }
    if (w != sz) { Serial.printf("storage:   short write %s\n", path); return false; }
    // read back
    f = LittleFS.open(path, "r");
    uint32_t c = 0, got = 0; uint8_t chunk[512];
    while (f && f.available()) { int r = f.read(chunk, sizeof(chunk)); if (r <= 0) break; c = esp_rom_crc32_le(c, chunk, r); got += r; }
    if (f) f.close();
    if (trace) { Serial.printf("storage:   [%u] verified %u\n", (unsigned)n, (unsigned)got); Serial.flush(); }
    if (got != sz || c != esp_rom_crc32_le(0, data, sz)) { Serial.printf("storage:   verify failed %s\n", path); return false; }
    n++; s_files++; s_bytes += sz;
    if (n % 25 == 0) Serial.printf("storage:   wrote %u/%u files (%u ms)\n", (unsigned)n, (unsigned)nfiles, (unsigned)millis());
  }
  if (n != nfiles) { free(blobs); return false; }
  bool bok = true;
  for (int sh = 0; sh < MXB_SHARDS && bok; sh++) bok = mxbWriteShard(LittleFS, blobs, sh);
  free(blobs);
  if (!bok) return false;
  Serial.printf("storage:   %d adverts into %d shard files\n", nb, MXB_SHARDS);
  File m = LittleFS.open(MIG_MARKER, "w", true);
  if (!m) return false;
  m.write((const uint8_t*)&crc, 4); m.close();
  return markerMatches(crc);
}

// Leftover /bl folder from an earlier layout: delete it a piece at a time (each step can take ~1-2 s on a
// big folder, so the UI only calls this while the screen is locked and idle). Returns true while work remains.
static bool s_bl_done = false;
static bool rmOne(const char* dir) {
  DIR* d = opendir(dir);
  if (!d) return false;
  struct dirent* e; char path[300]; bool any = false;
  while ((e = readdir(d)) != nullptr) {
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    any = true;
    if (e->d_type == DT_DIR) { closedir(d); if (!rmOne(path)) rmdir(path); return true; }
    closedir(d); unlink(path); return true;
  }
  closedir(d);
  return any;
}
bool mxStorageIdleCleanup() {
  if (s_bl_done) return false;
  if (rmOne("/littlefs/bl")) return true;
  rmdir("/littlefs/bl");
  s_bl_done = true;
  Serial.println("storage: old /bl folder removed");
  return false;
}

bool mxStorageBegin(DisplayDriver* disp) {
  const esp_partition_t* sp = scratchPart();
  MigHdr h; bool have_bundle = readHdr(sp, h);

  Serial.printf("storage: start, safety copy %s (%u ms)\n", have_bundle ? "PRESENT" : "none", (unsigned)millis());
  // Normal case: already LittleFS.
  if (LittleFS.begin(false)) {
    Serial.printf("storage: LittleFS mounted (%u ms)\n", (unsigned)millis());
    if (have_bundle) {
      if (markerMatches(h.crc)) { clearScratch(sp, h.len); }        // finished, header just wasn't erased yet
      else {                                                  // cut off while writing files back: redo
        show(disp, "Finishing storage upgrade", "Do not turn off");
        uint8_t* buf = loadScratch(sp, h);
        if (buf && restore(buf, h.len, h.nfiles, h.crc)) { clearScratch(sp, h.len); s_result = 2; }
        if (buf) free(buf);
        return true;
      }
    }
    s_result = 0;
    return true;
  }

  // Old SPIFFS data (and no bundle yet): migrate.
  if (!have_bundle && SPIFFS.begin(false)) {
    show(disp, "Upgrading storage...", "Do not turn off (1 min)");
    uint32_t len = 0, nfiles = 0;
    uint8_t* buf = collectSpiffs(len, nfiles);
    SPIFFS.end();
    if (!buf) {
      Serial.println("storage: could not read SPIFFS files - leaving them untouched");
      s_result = -1;
      return false;                                           // don't format: keep the user's data
    }
    uint32_t crc = esp_rom_crc32_le(0, buf, len);
    Serial.printf("storage: %u files, %u bytes, crc %08x\n", (unsigned)nfiles, (unsigned)len, (unsigned)crc);
    Serial.printf("storage: saving safety copy (%u ms)\n", (unsigned)millis());
    if (!saveScratch(sp, buf, len, nfiles, crc)) {
      Serial.println("storage: could not stage a safety copy - leaving SPIFFS untouched");
      free(buf); s_result = -1;
      return false;
    }
    Serial.printf("storage: safety copy OK, formatting LittleFS (%u ms)\n", (unsigned)millis());
    if (!LittleFS.begin(true)) { free(buf); s_result = -1; return false; }   // formats the partition
    Serial.printf("storage: LittleFS mounted, writing files back (%u ms)\n", (unsigned)millis());
    bool ok = restore(buf, len, nfiles, crc);
    free(buf);
    if (ok) { clearScratch(sp, len); s_result = 1; Serial.println("storage: migrated to LittleFS"); }
    else Serial.println("storage: write-back failed - will retry from the safety copy at next start");
    return true;
  }

  // Neither mounts: either a cut during formatting (bundle present) or a blank/new partition.
  if (have_bundle) {
    show(disp, "Finishing storage upgrade", "Do not turn off");
    uint8_t* buf = loadScratch(sp, h);
    if (!buf) { s_result = -1; return false; }               // bad copy: don't touch anything
    Serial.printf("storage: formatting LittleFS (%u ms)\n", (unsigned)millis());
    bool mounted = LittleFS.begin(true);
    bool ok = mounted && restore(buf, h.len, h.nfiles, h.crc);
    free(buf);
    if (ok) { clearScratch(sp, h.len); s_result = 2; }
    return mounted;
  }
  s_result = 3;
  return LittleFS.begin(true);                                // new device: format
}
