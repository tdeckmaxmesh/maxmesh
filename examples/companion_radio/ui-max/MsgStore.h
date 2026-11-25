#pragma once

// On-device message history for the T-Deck MAX UI.
// A ring buffer in PSRAM, persisted lazily to SPIFFS.

#include <Arduino.h>
#include <FS.h>

#define MSG_TEXT_MAX   168
#define MSG_AUTHOR_MAX 24
#ifndef MSG_STORE_SIZE
  #define MSG_STORE_SIZE 400
#endif

enum ConvKind : uint8_t { CONV_DM = 0, CONV_CHANNEL = 1 };

enum MsgStatus : uint8_t {
  MS_NONE = 0,        // incoming
  MS_SENDING,         // waiting for ack
  MS_SENT,            // channel message / no ack expected
  MS_DELIVERED,
  MS_FAILED
};

#define MF_OUT     0x01
#define MF_UNREAD  0x02
#define MF_ROOM    0x04   // post from a room server (author = original poster)
#define MF_CLI     0x08   // CLI reply from a repeater (shown in admin, not chats)

struct ConvKey {
  uint8_t kind;           // ConvKind
  uint8_t id[6];          // DM: pub-key prefix; channel: first 6 bytes of channel secret
  bool operator==(const ConvKey& o) const { return kind == o.kind && memcmp(id, o.id, 6) == 0; }
};

struct StoredMsg {
  uint32_t ts;            // our RTC time (secs) when stored
  uint32_t sender_ts;     // sender's timestamp
  ConvKey  conv;
  uint8_t  flags;
  uint8_t  status;
  uint8_t  hops;          // 0xFF = direct
  uint8_t  attempts;
  int8_t   snr4;          // SNR x4
  uint32_t ack;           // expected ack crc (outgoing DMs)
  uint32_t retry_at;      // millis() deadline for ack
  char     author[MSG_AUTHOR_MAX];
  char     text[MSG_TEXT_MAX];
};

class MsgStore {
  StoredMsg* _msgs = nullptr;
  int _count = 0;         // number of valid entries
  int _head = 0;          // index of oldest
  bool _dirty = false;
  uint32_t _dirty_since = 0;
  fs::FS* _fs = nullptr;

public:
  bool begin(fs::FS& fs);
  int count() const { return _count; }
  // i = 0 is oldest, count()-1 is newest
  StoredMsg* at(int i) { return (i < 0 || i >= _count) ? nullptr : &_msgs[(_head + i) % MSG_STORE_SIZE]; }
  StoredMsg* add();       // returns zeroed slot (evicts oldest when full)
  void markDirty() { if (!_dirty) _dirty_since = millis(); _dirty = true; }
  void markRead(const ConvKey& k);
  int  unreadFor(const ConvKey& k);
  int  totalUnread();
  void eraseConv(const ConvKey& k);
  StoredMsg* findByAck(uint32_t ack);
  void saveIfNeeded(bool force = false);
};
