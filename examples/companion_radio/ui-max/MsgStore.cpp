#include "MsgStore.h"
#include <esp_heap_caps.h>

#define MSG_FILE    "/mx_msgs"
#define MSG_TMP     "/mx_msgs.tmp"
#define MSG_MAGIC   0x4D584D31   // "MXM1"

struct FileHeader {
  uint32_t magic;
  uint16_t rec_size;
  uint16_t count;
};

bool MsgStore::begin(fs::FS& fs) {
  _fs = &fs;
  _msgs = (StoredMsg*) heap_caps_calloc(MSG_STORE_SIZE, sizeof(StoredMsg), MALLOC_CAP_SPIRAM);
  if (!_msgs) _msgs = (StoredMsg*) calloc(MSG_STORE_SIZE / 4, sizeof(StoredMsg));   // should never happen
  if (!_msgs) return false;
  _count = 0; _head = 0;

  // a reset between "remove old" and "rename new" leaves only the finished temp copy
  if (!fs.exists(MSG_FILE) && fs.exists(MSG_TMP)) fs.rename(MSG_TMP, MSG_FILE);

  File f = fs.open(MSG_FILE, "r");
  if (f) {
    FileHeader h;
    if (f.read((uint8_t*)&h, sizeof(h)) == sizeof(h) && h.magic == MSG_MAGIC && h.rec_size == sizeof(StoredMsg)) {
      int n = min((int)h.count, MSG_STORE_SIZE);
      for (int i = 0; i < n; i++) {
        if (f.read((uint8_t*)&_msgs[i], sizeof(StoredMsg)) != sizeof(StoredMsg)) break;
        StoredMsg& m = _msgs[i];
        m.text[MSG_TEXT_MAX - 1] = 0;
        m.author[MSG_AUTHOR_MAX - 1] = 0;
        if (m.status == MS_SENDING) m.status = MS_FAILED;   // reboot interrupted it
        _count++;
      }
    }
    f.close();
  }
  return true;
}

StoredMsg* MsgStore::add() {
  int idx;
  if (_count < MSG_STORE_SIZE) {
    idx = (_head + _count) % MSG_STORE_SIZE;
    _count++;
  } else {
    idx = _head;                    // overwrite oldest
    _head = (_head + 1) % MSG_STORE_SIZE;
  }
  memset(&_msgs[idx], 0, sizeof(StoredMsg));
  markDirty();
  return &_msgs[idx];
}

void MsgStore::markRead(const ConvKey& k) {
  bool any = false;
  for (int i = 0; i < _count; i++) {
    StoredMsg* m = at(i);
    if ((m->flags & MF_UNREAD) && m->conv == k) { m->flags &= ~MF_UNREAD; any = true; }
  }
  if (any) markDirty();
}

int MsgStore::unreadFor(const ConvKey& k) {
  int n = 0;
  for (int i = 0; i < _count; i++) {
    StoredMsg* m = at(i);
    if ((m->flags & MF_UNREAD) && m->conv == k) n++;
  }
  return n;
}

int MsgStore::totalUnread() {
  int n = 0;
  for (int i = 0; i < _count; i++) if (at(i)->flags & MF_UNREAD) n++;
  return n;
}

void MsgStore::eraseConv(const ConvKey& k) {
  // compact in place, keeping order
  int w = 0;
  for (int r = 0; r < _count; r++) {
    StoredMsg* src = at(r);
    if (src->conv == k) continue;
    StoredMsg* dst = &_msgs[(_head + w) % MSG_STORE_SIZE];
    if (dst != src) *dst = *src;
    w++;
  }
  _count = w;
  markDirty();
}

StoredMsg* MsgStore::findByAck(uint32_t ack) {
  if (ack == 0) return nullptr;
  for (int i = _count - 1; i >= 0; i--) {
    StoredMsg* m = at(i);
    if ((m->flags & MF_OUT) && m->ack == ack) return m;
  }
  return nullptr;
}

void MsgStore::saveIfNeeded(bool force) {
  if (!_dirty || !_fs) return;
  if (!force && millis() - _dirty_since < 45000) return;   // batch writes
  // Write a complete new copy first, then swap it in: a reset or flat battery in
  // the middle of a save must never destroy the existing history.
  File f = _fs->open(MSG_TMP, "w", true);
  if (!f) return;
  FileHeader h = { MSG_MAGIC, (uint16_t)sizeof(StoredMsg), (uint16_t)_count };
  size_t want = sizeof(h) + (size_t)_count * sizeof(StoredMsg), wrote = 0;
  wrote += f.write((uint8_t*)&h, sizeof(h));
  for (int i = 0; i < _count; i++) wrote += f.write((uint8_t*)at(i), sizeof(StoredMsg));
  f.close();
  if (wrote != want) { _fs->remove(MSG_TMP); return; }   // storage full etc: keep the old file
  if (!_fs->rename(MSG_TMP, MSG_FILE)) {   // LittleFS replaces atomically; SPIFFS needs the old one gone
    _fs->remove(MSG_FILE);
    _fs->rename(MSG_TMP, MSG_FILE);
  }
  _dirty = false;
}
