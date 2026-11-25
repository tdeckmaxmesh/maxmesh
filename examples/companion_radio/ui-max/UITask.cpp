// T-Deck MAX standalone UI for MeshCore
//
// Key conventions (no arrows on this keyboard):
//   lists:   W/S = up/down, A/D = page/tab, Enter = open, Q or Backspace = back
//   typing:  letters go into the text box, Enter = send/OK, Backspace = delete
//            (Backspace on an empty box = back), Alt+W/S = scroll, Alt+Q = back
//   global:  BOOT button short = lock/unlock, hold 3s = power off
//            Alt+L = lock, Alt+B = frontlight, Alt+H = home

#include "UITask.h"
#include <SPIFFS.h>
#include <stdarg.h>
#include <time.h>
// base64.hpp is header-only (already compiled into BaseChatMesh.cpp), so just declare what we use
unsigned int encode_base64(const unsigned char* input, unsigned int input_length, unsigned char* output);
unsigned int decode_base64(const unsigned char* input, unsigned int input_length, unsigned char* output);
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include <Fonts/FreeSansBold24pt7b.h>

extern UITask ui_task;

// The main loop can get deep: an e-paper refresh keeps the mesh running from its busy
// callback, so drawing + packet handling + contact/file saves can all be on the stack at
// once. 8 KB (Arduino default) overflowed in the field; give it 16 KB.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ---- crash breadcrumbs: survive a panic/watchdog reboot (not a power loss) ----
#include <esp_system.h>
#define BC_MAGIC 0xB4EADC0D
RTC_NOINIT_ATTR static uint32_t bc_magic, bc_key, bc_scr, bc_stage, bc_modal, bc_crashes;
static uint32_t bc_prev_key, bc_prev_scr, bc_prev_stage, bc_prev_modal;
static int bc_reason = -1;
enum { BC_IDLE = 0, BC_KEY = 1, BC_RENDER = 2, BC_MESH_IN_REFRESH = 3, BC_SAVE = 4, BC_SLEEP = 5 };
static inline void bcMark(uint32_t stage) { bc_stage = stage; }

#define W 240
#define H 320
#define HDR_H 18
#define FTR_H 13
#define BLACK GxEPD_BLACK
#define WHITE GxEPD_WHITE
#define UI_CFG_FILE "/mx_ui.cfg"
#define UI_CFG_MAGIC 0x4D585533

// RepeaterStats layout as sent by simple_repeater / room server (REQ_TYPE_GET_STATUS)
struct RemoteStats {
  uint16_t batt_milli_volts;
  uint16_t curr_tx_queue_len;
  int16_t  noise_floor;
  int16_t  last_rssi;
  uint32_t n_packets_recv;
  uint32_t n_packets_sent;
  uint32_t total_air_time_secs;
  uint32_t total_up_time_secs;
  uint32_t n_sent_flood, n_sent_direct;
  uint32_t n_recv_flood, n_recv_direct;
  uint16_t err_events;
  int16_t  last_snr;   // x4
  uint16_t n_direct_dups, n_flood_dups;
};

struct RadioPreset { const char* name; float freq; float bw; uint8_t sf; uint8_t cr; };
static const RadioPreset PRESETS[] = {
  { "USA/Canada",   910.525f, 62.5f, 7, 5 },
  { "EU/UK Narrow", 869.618f, 62.5f, 8, 8 },
};
#define NUM_PRESETS (int)(sizeof(PRESETS) / sizeof(PRESETS[0]))
static const float BWS[] = { 7.8f, 10.4f, 15.6f, 20.8f, 31.25f, 41.7f, 62.5f, 125.0f, 250.0f, 500.0f };
static const uint16_t LOCK_OPTS[] = { 30, 60, 120, 300, 600, 0 };
static const char* LIGHT_NAMES[] = { "Off", "Low", "Medium", "High" };
static const uint8_t LIGHT_LEVELS[] = { 0, 20, 70, 200 };
static const char* VIBE_NAMES[] = { "Off", "DMs only", "All messages" };
static const char* FILTER_NAMES[] = { "All", "People", "Repeaters", "Rooms", "Favs" };

enum { NUM_SETTINGS = 23 };

// ======================================================================
//  small utilities
// ======================================================================

// Copy UTF-8 into the printable ASCII the GFX fonts can draw.
static void toAscii(char* dst, const char* src, int n) {
  int j = 0;
  for (int i = 0; src[i] && j < n - 1; i++) {
    uint8_t c = (uint8_t)src[i];
    if (c >= 32 && c < 127) { dst[j++] = c; continue; }
    if (c == '\n') { dst[j++] = ' '; continue; }
    if (c >= 0xC0) {
      // decode a few common punctuation code points, anything else -> '?'
      uint32_t cp = 0; int extra = 0;
      if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
      else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
      else { cp = c & 0x07; extra = 3; }
      for (int e = 0; e < extra && src[i + 1]; e++) cp = (cp << 6) | (src[++i] & 0x3F);
      char r = '?';
      if (cp == 0x2018 || cp == 0x2019) r = '\'';
      else if (cp == 0x201C || cp == 0x201D) r = '"';
      else if (cp == 0x2013 || cp == 0x2014) r = '-';
      else if (cp == 0x2026) r = '.';
      else if (cp == 0xB0) r = 'o';
      else if (cp >= 0x1F300) r = '*';           // emoji
      else if (cp == 0xFE0F || cp == 0x200D) continue;
      dst[j++] = r;
    }
  }
  dst[j] = 0;
}

static bool hasPrefix(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

static void hexStr(char* out, const uint8_t* b, int n) {
  static const char* hx = "0123456789ABCDEF";
  for (int i = 0; i < n; i++) { out[i * 2] = hx[b[i] >> 4]; out[i * 2 + 1] = hx[b[i] & 15]; }
  out[n * 2] = 0;
}

// days from civil (Howard Hinnant)
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void ui_display_busy_hook() { ui_task.busyPoll(); }

// ======================================================================
//  setup / config
// ======================================================================

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  // read crash breadcrumbs left by the previous run
  bc_reason = (int)esp_reset_reason();
  if (bc_magic == BC_MAGIC) {
    bc_prev_key = bc_key; bc_prev_scr = bc_scr; bc_prev_stage = bc_stage; bc_prev_modal = bc_modal;
    if (bc_reason == ESP_RST_PANIC || bc_reason == ESP_RST_INT_WDT || bc_reason == ESP_RST_TASK_WDT ||
        bc_reason == ESP_RST_WDT || bc_reason == ESP_RST_BROWNOUT) bc_crashes++;
  } else {
    bc_crashes = 0; bc_prev_key = bc_prev_scr = bc_prev_stage = bc_prev_modal = 0;
  }
  bc_magic = BC_MAGIC; bc_key = 0; bc_scr = 0; bc_stage = BC_IDLE; bc_modal = 0;

  _disp = (TDeckMaxDisplay*)display;
  _sensors = sensors;
  _prefs = node_prefs;
  _input[0] = 0; _find[0] = 0; _toast[0] = 0; _pw_cache[0] = 0;
  memset(&_adm, 0, sizeof(_adm));

  loadCfg();
  _store.begin(SPIFFS);
  spell.begin(SPIFFS);

  // keep the BLE PIN stable across reboots (upstream picks a new random one each boot)
  if (_prefs->ble_pin == 0 && the_mesh.getBLEPin() != 0) {
    _prefs->ble_pin = the_mesh.getBLEPin();
    the_mesh.savePrefs();
  }
  keyboard.begin();
  touch.begin();

  if (!_cfg.ble_on) disableBluetooth(); else enableBluetooth();
  _disp->setFullRefreshEvery(_cfg.full_every);
  applyLights();
  gpsPowerSync();
  readBattery();

  _boot_ms = millis();
  _last_input = millis();
  _need_full = true;
  _dirty = true;

  if (!_cfg.welcomed) {
    _cfg.welcomed = 1;
    saveCfg();
    openInput(P_SET_NAME, "Welcome to maxmesh! Your name:", _prefs->node_name, 31);
  }
}

void UITask::loadCfg() {
  memset(&_cfg, 0, sizeof(_cfg));
  File f = SPIFFS.open(UI_CFG_FILE, "r");
  bool ok = false;
  if (f) { ok = f.read((uint8_t*)&_cfg, sizeof(_cfg)) == sizeof(_cfg) && _cfg.magic == UI_CFG_MAGIC; f.close(); }
  if (!ok) {
    memset(&_cfg, 0, sizeof(_cfg));
    _cfg.magic = UI_CFG_MAGIC;
    _cfg.ble_on = 1;
    _cfg.vibrate = 2;
    _cfg.frontlight = 0;
    _cfg.kb_light = 0;
    _cfg.lock_secs = 120;
    _cfg.full_every = 25;
    _cfg.welcomed = 0;
  }
  if (_cfg.full_every < 5) _cfg.full_every = 25;
}

void UITask::saveCfg() {
  File f = SPIFFS.open(UI_CFG_FILE, "w", true);
  if (f) { f.write((uint8_t*)&_cfg, sizeof(_cfg)); f.close(); }
}

void UITask::applyLights() {
  bool locked = _scr == SCR_LOCK;
  board.setFrontlight(locked ? 0 : LIGHT_LEVELS[_cfg.frontlight & 3]);
  board.setKeyboardLight(!locked && _cfg.kb_light);
}

void UITask::gpsPowerSync() {
  bool want = _prefs->gps_enabled;
  if (want != _gps_pow) {
    board.setGpsPower(want);
    _gps_pow = want;
  }
}

void UITask::readBattery() {
  _batt_mv = board.getBattMilliVolts();
  _batt_pct = board.getBattPercent();
  _batt_ma = board.getBattCurrentMA();
  _usb = board.isUsbPowered();
}

// ======================================================================
//  navigation & modals
// ======================================================================

void UITask::go(Screen s) {
  if (_sp < 10) _stack[_sp++] = _scr;
  _scr = s;
  _sel[s] = (s == SCR_CHAT) ? 0 : _sel[s];
  _dirty = true;
}

void UITask::back() {
  if (_sp > 0) _scr = _stack[--_sp];
  else _scr = SCR_HOME;
  _dirty = true;
}

void UITask::toast(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vsnprintf(_toast, sizeof(_toast), fmt, ap);
  va_end(ap);
  _toast_until = millis() + 3500;
  _dirty = true;
}

void UITask::openInput(Purpose p, const char* title, const char* init, int max_len, bool numeric) {
  _modal = MODAL_INPUT; _purpose = p;
  StrHelper::strncpy(_m_title, title, sizeof(_m_title));
  StrHelper::strncpy(_m_buf, init ? init : "", sizeof(_m_buf));
  _m_max = min(max_len, (int)sizeof(_m_buf) - 1);
  _m_numeric = numeric;
  _dirty = true;
}

void UITask::openConfirm(Purpose p, const char* text) {
  _modal = MODAL_CONFIRM; _purpose = p;
  StrHelper::strncpy(_m_text, text, sizeof(_m_text));
  _dirty = true;
}

void UITask::openPick(Purpose p, const char* title, int n, int sel) {
  _modal = MODAL_PICK; _purpose = p;
  StrHelper::strncpy(_m_title, title, sizeof(_m_title));
  _m_nopts = n;
  for (int i = 0; i < n; i++) _m_opts[i] = _m_optbuf[i];
  _m_sel = constrain(sel, 0, n - 1);
  _m_top = max(0, _m_sel - 4);
  _dirty = true;
}

void UITask::modalKey(int k) {
  if (_modal == MODAL_INPUT) {
    int len = strlen(_m_buf);
    if (k == MK_ENTER) { modalDone(true); return; }
    if (k == MK_BACK) { modalDone(false); return; }
    if (k == MK_BS) {
      if (len > 0) _m_buf[len - 1] = 0;          // (empty box: ignore - only Alt+Q cancels)
    } else if (k == MK_MIC && len < _m_max) {
      _m_buf[len] = '0'; _m_buf[len + 1] = 0;
    } else if (k >= 32 && k < 127 && len < _m_max) {
      _m_buf[len] = (char)k; _m_buf[len + 1] = 0;
    }
  } else if (_modal == MODAL_CONFIRM) {
    // info boxes close with anything; real questions need an explicit Y
    if (_purpose == P_NONE) { modalDone(false); return; }
    if (k == 'y' || k == 'Y') modalDone(true);
    else if (k == MK_BACK || k == MK_BS || k == 'n' || k == 'N' || k == 'q') modalDone(false);
  } else if (_modal == MODAL_PICK) {
    if (k == 'w' || k == MK_UP) _m_sel = (_m_sel + _m_nopts - 1) % _m_nopts;
    else if (k == 's' || k == MK_DOWN) _m_sel = (_m_sel + 1) % _m_nopts;
    else if (k == MK_ENTER || k == ' ') { modalDone(true); return; }
    else if (k == MK_BACK || k == MK_BS || k == 'q') { modalDone(false); return; }
    if (_m_sel < _m_top) _m_top = _m_sel;
    if (_m_sel >= _m_top + 9) _m_top = _m_sel - 8;
  }
  _dirty = true;
}

// ======================================================================
//  helpers: time, names, contacts
// ======================================================================

// An unset clock starts at the fallback seed (RTC_TIME_MIN) and just counts up from there.
bool UITask::clockValid() {
#ifdef RTC_TIME_MIN
  return rtc_clock.getCurrentTime() > (uint32_t)RTC_TIME_MIN + millis() / 1000 + 600;
#else
  return rtc_clock.getCurrentTime() > 1767225600UL;   // 2026-01-01
#endif
}

uint32_t UITask::localNow() { return rtc_clock.getCurrentTime() + (int32_t)_prefs->tz_offset * 3600; }

void UITask::fmtTime(uint32_t t, char* out) {
  if (!clockValid() || t < 1600000000UL) { strcpy(out, "--:--"); return; }
  time_t lt = t + (int32_t)_prefs->tz_offset * 3600;
  struct tm tmv; gmtime_r(&lt, &tmv);
  uint32_t now = localNow();
  if (now - (uint32_t)lt < 86400 && tmv.tm_mday == ({ time_t n = now; struct tm x; gmtime_r(&n, &x); x.tm_mday; }))
    sprintf(out, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  else
    sprintf(out, "%d/%d %02d:%02d", tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

void UITask::fmtAge(uint32_t t, char* out) {
  uint32_t now = rtc_clock.getCurrentTime();
  if (t == 0 || t > now + 60) { strcpy(out, "?"); return; }
  uint32_t d = now - t;
  if (d < 60) strcpy(out, "now");
  else if (d < 3600) sprintf(out, "%um", (unsigned)(d / 60));
  else if (d < 86400) sprintf(out, "%uh", (unsigned)(d / 3600));
  else sprintf(out, "%ud", (unsigned)(d / 86400));
}

ConvKey UITask::keyForContact(const ContactInfo& c) {
  ConvKey k; k.kind = CONV_DM; memcpy(k.id, c.id.pub_key, 6); return k;
}

ConvKey UITask::keyForChannel(int idx) {
  ConvKey k; k.kind = CONV_CHANNEL; memset(k.id, 0, 6);
  ChannelDetails ch;
  if (the_mesh.getChannel(idx, ch)) memcpy(k.id, ch.channel.secret, 6);
  return k;
}

int UITask::channelForKey(const ConvKey& k) {
  if (k.kind != CONV_CHANNEL) return -1;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails ch;
    if (the_mesh.getChannel(i, ch) && ch.name[0] && memcmp(ch.channel.secret, k.id, 6) == 0) return i;
  }
  return -1;
}

bool UITask::convName(const ConvKey& k, char* dest, int n) {
  if (k.kind == CONV_CHANNEL) {
    int i = channelForKey(k);
    if (i >= 0) { ChannelDetails ch; the_mesh.getChannel(i, ch); toAscii(dest, ch.name, n); return true; }
    StrHelper::strncpy(dest, "(deleted channel)", n);
    return false;
  }
  ContactInfo* c = the_mesh.lookupContactByPubKey(k.id, 6);
  if (c) { toAscii(dest, c->name, n); return true; }
  char hx[16]; hexStr(hx, k.id, 4);
  snprintf(dest, n, "<%s>", hx);
  return false;
}

ContactInfo* UITask::curContact() { return the_mesh.lookupContactByPubKey(_cur_pub, PUB_KEY_SIZE); }

static int s_sort_mode = 0;
static int cmpContacts(const void* a, const void* b) {
  ContactInfo ca, cb;
  the_mesh.getContactByIdx(*(const int*)a, ca);
  the_mesh.getContactByIdx(*(const int*)b, cb);
  if (ca.isFav() != cb.isFav()) return ca.isFav() ? -1 : 1;
  if (ca.lastmod != cb.lastmod) return ca.lastmod > cb.lastmod ? -1 : 1;
  return strcasecmp(ca.name, cb.name);
}

void UITask::buildContactList() {
  _list_n = 0;
  char lname[32], lfind[24];
  for (int i = 0; _find[i] && i < 23; i++) lfind[i] = tolower(_find[i]);
  lfind[min((int)strlen(_find), 23)] = 0;
  for (int i = MAX_ANON_CONTACTS; i < the_mesh.getTotalContactSlots() && _list_n < UI_MAX_LIST; i++) {
    ContactInfo c;
    if (!the_mesh.getContactByIdx(i, c) || c.type == ADV_TYPE_NONE || c.name[0] == 0) continue;
    bool ok = true;
    switch (_ct_filter) {
      case 1: ok = c.type == ADV_TYPE_CHAT; break;
      case 2: ok = c.type == ADV_TYPE_REPEATER; break;
      case 3: ok = c.type == ADV_TYPE_ROOM; break;
      case 4: ok = c.isFav(); break;
    }
    if (ok && lfind[0]) {
      int j = 0;
      for (; c.name[j] && j < 31; j++) lname[j] = tolower(c.name[j]);
      lname[j] = 0;
      ok = strstr(lname, lfind) != nullptr;
    }
    if (ok) _list_idx[_list_n++] = i;
  }
  qsort(_list_idx, _list_n, sizeof(int), cmpContacts);
  if (_sel[SCR_CONTACTS] >= _list_n) _sel[SCR_CONTACTS] = max(0, _list_n - 1);
}

void UITask::buildConvs() {
  _nconv = 0;
  for (int i = 0; i < _store.count(); i++) {
    StoredMsg* m = _store.at(i);
    if (m->flags & MF_CLI) continue;
    int j = 0;
    for (; j < _nconv; j++) if (_convs[j].key == m->conv) break;
    if (j == _nconv) {
      if (_nconv >= 64) continue;
      _convs[j].key = m->conv; _convs[j].unread = 0;
      _nconv++;
    }
    _convs[j].last = m->ts;
    _convs[j].last_idx = i;
    if (m->flags & MF_UNREAD) _convs[j].unread++;
  }
  // newest first
  for (int a = 1; a < _nconv; a++) {
    ConvEntry t = _convs[a]; int b = a - 1;
    while (b >= 0 && _convs[b].last_idx < t.last_idx) { _convs[b + 1] = _convs[b]; b--; }
    _convs[b + 1] = t;
  }
}

void UITask::openChat(const ConvKey& k) {
  _chat = k;
  _scroll = 0;
  _input[0] = 0;
  _chat_room_ch = channelForKey(k);
  // remember where the unseen messages start, for the "New Messages" divider
  _new_marker = nullptr; _new_marker_ts = 0;
  int unread = 0;
  for (int i = 0; i < _store.count(); i++) {
    StoredMsg* m = _store.at(i);
    if (!(m->conv == k) || (m->flags & MF_CLI)) continue;
    if (m->flags & MF_UNREAD) {
      if (!_new_marker) { _new_marker = m; _new_marker_ts = m->ts; }
      unread++;
    }
  }
  if (unread > 4) _scroll = unread - 4;   // open at the first new message, not the bottom
  _store.markRead(k);
  go(SCR_CHAT);
}

// ======================================================================
//  sending
// ======================================================================

bool UITask::sendChat() {
  if (!_input[0]) return false;
  uint32_t ts = rtc_clock.getCurrentTimeUnique();
  StoredMsg* m = nullptr;

  if (_chat.kind == CONV_CHANNEL) {
    int idx = channelForKey(_chat);
    ChannelDetails ch;
    if (idx < 0 || !the_mesh.getChannel(idx, ch)) { toast("Channel not found"); return false; }
    if (!the_mesh.sendGroupMessage(ts, ch.channel, _prefs->node_name, _input, strlen(_input))) {
      toast("Send failed (queue full)"); return false;
    }
    m = _store.add();
    m->status = MS_SENT;
  } else {
    ContactInfo* c = the_mesh.lookupContactByPubKey(_chat.id, 6);
    if (!c) { toast("Contact not found"); return false; }
    uint32_t ack = 0, est = 0;
    int r = the_mesh.sendMessage(*c, ts, 0, _input, ack, est);
    if (r == MSG_SEND_FAILED) { toast("Send failed (queue full)"); return false; }
    m = _store.add();
    m->status = MS_SENDING;
    m->ack = ack;
    m->retry_at = millis() + est + 1500;
    m->hops = (r == MSG_SEND_SENT_FLOOD) ? 1 : 0;
  }
  m->ts = rtc_clock.getCurrentTime();
  m->sender_ts = ts;
  m->conv = _chat;
  m->flags = MF_OUT;
  StrHelper::strncpy(m->author, _prefs->node_name, MSG_AUTHOR_MAX);
  StrHelper::strncpy(m->text, _input, MSG_TEXT_MAX);
  _input[0] = 0;
  _scroll = 0;
  return true;
}

void UITask::retryLoop() {
  uint32_t now = millis();
  for (int i = _store.count() - 1, seen = 0; i >= 0 && seen < 40; i--, seen++) {
    StoredMsg* m = _store.at(i);
    if (!(m->flags & MF_OUT) || m->status != MS_SENDING) continue;
    if ((int32_t)(now - m->retry_at) < 0) continue;
    ContactInfo* c = the_mesh.lookupContactByPubKey(m->conv.id, 6);
    if (!c || m->attempts >= 3) { m->status = MS_FAILED; _store.markDirty(); _dirty = true; continue; }
    m->attempts++;
    if (m->attempts == 3) the_mesh.resetPathTo(*c);   // last try: flood
    uint32_t ack = 0, est = 0;
    int r = the_mesh.sendMessage(*c, m->sender_ts, m->attempts, m->text, ack, est);
    if (r == MSG_SEND_FAILED) { m->retry_at = now + 3000; continue; }
    m->ack = ack;
    m->retry_at = now + est + 1500;
  }
}

void UITask::doAdvert(bool flood) {
  bool ok = flood ? the_mesh.uiFloodAdvert() : the_mesh.advert();
  if (ok) board.buzz(1);
  toast(ok ? (flood ? "Flood advert sent" : "Advert sent (zero hop)") : "Advert failed");
}

void UITask::sendLogin(ContactInfo& c, const char* pw) {
  uint32_t est = 0;
  int r = the_mesh.sendLogin(c, pw, est);
  if (r == MSG_SEND_FAILED) { toast("Login send failed"); return; }
  _pend_kind = 1;
  memcpy(_pend_pub, c.id.pub_key, 4);
  _pend_deadline = millis() + est + 6000;
  StrHelper::strncpy(_pw_cache, pw, sizeof(_pw_cache));
  admLog(r == MSG_SEND_SENT_FLOOD ? "> login (flood)..." : "> login (direct)...");
}

void UITask::requestStatus(ContactInfo& c) {
  uint32_t tag = 0, est = 0;
  int r = the_mesh.sendRequest(c, REQ_TYPE_GET_STATUS, tag, est);
  if (r == MSG_SEND_FAILED) { toast("Status request failed"); return; }
  _pend_kind = 2;
  memcpy(_pend_pub, c.id.pub_key, 4);
  _pend_deadline = millis() + est + 6000;
  admLog("> status request...");
}

void UITask::sendCli(const char* cmd) {
  ContactInfo* c = curContact();
  if (!c) return;
  uint32_t est = 0;
  uint32_t ts = rtc_clock.getCurrentTimeUnique();
  int r = the_mesh.sendCommandData(*c, ts, 0, TXT_TYPE_CLI_DATA, cmd, est);
  char line[120];
  snprintf(line, sizeof(line), "> %s%s", cmd, r == MSG_SEND_FAILED ? "  (send failed)" : "");
  admLog(line);
}

void UITask::admLog(const char* line) {
  if (_adm.log_n == 6) {
    for (int i = 1; i < 6; i++) memcpy(_adm.log[i - 1], _adm.log[i], sizeof(_adm.log[0]));
    _adm.log_n = 5;
  }
  toAscii(_adm.log[_adm.log_n++], line, sizeof(_adm.log[0]));
  _dirty = true;
}

void UITask::applyPreset(int i) {
  if (i < 0 || i >= NUM_PRESETS) return;
  _prefs->freq = PRESETS[i].freq;
  _prefs->bw = PRESETS[i].bw;
  _prefs->sf = PRESETS[i].sf;
  _prefs->cr = PRESETS[i].cr;
  the_mesh.uiApplyRadioParams();
  toast("Radio: %s", PRESETS[i].name);
}

void UITask::notifyIncoming(bool is_dm, const char* from, const char* text) {
  bool viewing = _scr == SCR_CHAT && _modal == MODAL_NONE;
  if (_cfg.vibrate == 2 || (_cfg.vibrate == 1 && is_dm)) board.buzz(is_dm ? 14 : 1);
  if (_scr != SCR_LOCK && !viewing) {
    char a[24]; toAscii(a, from, sizeof(a));
    toast("%s: %s", a, text);
  }
  _dirty = true;
}

// ======================================================================
//  mesh callbacks
// ======================================================================

void UITask::notify(UIEventType t) { _dirty = true; }

void UITask::onMessageRecv(mesh::Packet* pkt, const ContactInfo& from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) {
  if (txt_type == TXT_TYPE_CLI_DATA) {
    // reply from a repeater/room we're administering
    if (memcmp(from.id.pub_key, _adm.pub, 6) == 0) {
      char buf[120];
      snprintf(buf, sizeof(buf), "%s", text);
      admLog(buf);
    }
    return;
  }
  if (txt_type == TXT_TYPE_SIGNED_PLAIN) return;   // handled by onSignedMessageRecv
  ConvKey k = keyForContact(from);
  // drop duplicates (sender retries we already have)
  for (int i = _store.count() - 1, n = 0; i >= 0 && n < 30; i--, n++) {
    StoredMsg* m = _store.at(i);
    if (!(m->flags & MF_OUT) && m->conv == k && m->sender_ts == sender_timestamp && strncmp(m->text, text, MSG_TEXT_MAX - 1) == 0) return;
  }
  StoredMsg* m = _store.add();
  m->ts = rtc_clock.getCurrentTime();
  m->sender_ts = sender_timestamp;
  m->conv = k;
  bool viewing = _scr == SCR_CHAT && _chat == k && _modal == MODAL_NONE;
  m->flags = viewing ? 0 : MF_UNREAD;
  m->hops = pkt->isRouteFlood() ? pkt->getPathHashCount() : 0xFF;
  m->snr4 = (int8_t)(pkt->getSNR() * 4);
  StrHelper::strncpy(m->author, from.name, MSG_AUTHOR_MAX);
  StrHelper::strncpy(m->text, text, MSG_TEXT_MAX);
  notifyIncoming(true, from.name, text);
}

void UITask::onSignedMessageRecv(mesh::Packet* pkt, const ContactInfo& from, uint32_t sender_timestamp, const uint8_t* author_prefix, const char* text) {
  ConvKey k = keyForContact(from);
  StoredMsg* m = _store.add();
  m->ts = rtc_clock.getCurrentTime();
  m->sender_ts = sender_timestamp;
  m->conv = k;
  bool viewing = _scr == SCR_CHAT && _chat == k && _modal == MODAL_NONE;
  m->flags = MF_ROOM | (viewing ? 0 : MF_UNREAD);
  m->hops = pkt->isRouteFlood() ? pkt->getPathHashCount() : 0xFF;
  ContactInfo* a = the_mesh.lookupContactByPubKey(author_prefix, 4);
  if (a) StrHelper::strncpy(m->author, a->name, MSG_AUTHOR_MAX);
  else if (memcmp(author_prefix, the_mesh.self_id.pub_key, 4) == 0) StrHelper::strncpy(m->author, _prefs->node_name, MSG_AUTHOR_MAX);
  else { char hx[12]; hexStr(hx, author_prefix, 4); StrHelper::strncpy(m->author, hx, MSG_AUTHOR_MAX); }
  StrHelper::strncpy(m->text, text, MSG_TEXT_MAX);
  notifyIncoming(false, m->author, text);
}

void UITask::onChannelMessageRecv(mesh::Packet* pkt, ChannelDetails& channel, const char* text) {
  ConvKey k; k.kind = CONV_CHANNEL; memcpy(k.id, channel.channel.secret, 6);
  StoredMsg* m = _store.add();
  m->ts = rtc_clock.getCurrentTime();
  m->conv = k;
  bool viewing = _scr == SCR_CHAT && _chat == k && _modal == MODAL_NONE;
  m->flags = viewing ? 0 : MF_UNREAD;
  m->hops = pkt->isRouteFlood() ? pkt->getPathHashCount() : 0xFF;
  m->snr4 = (int8_t)(pkt->getSNR() * 4);
  // channel text is "<sender>: <message>"
  const char* sep = strstr(text, ": ");
  if (sep && sep - text < MSG_AUTHOR_MAX) {
    memcpy(m->author, text, sep - text); m->author[sep - text] = 0;
    StrHelper::strncpy(m->text, sep + 2, MSG_TEXT_MAX);
  } else {
    StrHelper::strncpy(m->text, text, MSG_TEXT_MAX);
  }
  char who[48]; snprintf(who, sizeof(who), "%s%s %s", channel.name[0] == '#' ? "" : "#", channel.name, m->author);
  notifyIncoming(false, who, m->text);
}

void UITask::onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) {
  if (is_new) {
    char n[24]; toAscii(n, contact.name, sizeof(n));
    if (_scr != SCR_LOCK) toast("New contact: %s", n);
  }
  if (_scr == SCR_CONTACTS || _scr == SCR_HEARD) _dirty = true;
}

void UITask::onACKRecv(uint32_t ack_crc) {
  StoredMsg* m = _store.findByAck(ack_crc);
  if (m && m->status != MS_DELIVERED) {
    m->status = MS_DELIVERED;
    _store.markDirty();
    _dirty = true;
  }
}

bool UITask::onUnhandledResponse(const ContactInfo& from, uint32_t tag, const uint8_t* data, uint8_t len) {
  if (_pend_kind == 0 || memcmp(from.id.pub_key, _pend_pub, 4) != 0) return false;
  if (_pend_kind == 1) {               // login response
    _pend_kind = 0;
    bool ok = false, admin = false;
    if (len >= 2 && memcmp(data, "OK", 2) == 0) { ok = true; }
    else if (len >= 3 && data[0] == RESP_SERVER_LOGIN_OK) {
      ok = true;
      admin = (data[2] & 0x01) != 0;
      uint16_t keep_alive = ((uint16_t)data[1]) * 16;
      if (keep_alive > 0) the_mesh.uiStartConnection(from, keep_alive);
    }
    memcpy(_adm.pub, from.id.pub_key, 6);
    _adm.logged_in = ok;
    _adm.is_admin = admin;
    admLog(ok ? (admin ? "Logged in (admin)" : "Logged in (guest)") : "Login FAILED (wrong password?)");
    board.buzz(ok ? 1 : 14);
    return true;
  }
  if (_pend_kind == 2) {               // status response
    _pend_kind = 0;
    _adm.has_status = true;
    _adm.status_len = min((int)len, (int)sizeof(_adm.status));
    memcpy(_adm.status, data, _adm.status_len);
    _adm.status_time = rtc_clock.getCurrentTime();
    admLog("Status received");
    return true;
  }
  return false;
}

// ======================================================================
//  main loop
// ======================================================================

// The ESP32-S3's USB-serial link doesn't come back after light sleep, so the PC
// can't see the device. When USB power appears after we've slept, save and restart
// (takes ~3 s; the screen keeps showing the last image meanwhile).
void UITask::usbPluggedAfterSleep() {
  _store.saveIfNeeded(true);
  the_mesh.savePrefs();
  delay(50);
  ESP.restart();
}

void UITask::busyPoll() {
  if (_in_busy || _boot_ms == 0) return;
  _in_busy = true;
  uint32_t st = bc_stage; bcMark(BC_MESH_IN_REFRESH);
  the_mesh.loop();
  rtc_clock.tick();
  bcMark(st);
  _in_busy = false;
}

void UITask::handleButton() {
  bool down = digitalRead(MAX_BOOT_BTN) == LOW;
  uint32_t now = millis();
  if (down && !_btn_prev) { _btn_down = now; _btn_long_done = false; }
  if (down && !_btn_long_done && now - _btn_down > 2500) {
    _btn_long_done = true;
    // power off
    _store.saveIfNeeded(true);
    the_mesh.savePrefs();
    _disp->startFrame();
    g().setTextColor(BLACK);
    font(3); g().setCursor(20, 140); g().print("Powered off");
    font(1); g().setCursor(20, 175); g().print("Press the BOOT button");
    g().setCursor(20, 195); g().print("to turn back on.");
    _disp->forceFullRefresh();
    _disp->commit();
    board.powerOff();
  }
  if (!down && _btn_prev && !_btn_long_done && now - _btn_down > 30) {
    if (_scr == SCR_LOCK) back(); else { go(SCR_LOCK); _need_full = true; }
    applyLights();
    _last_input = now;
  }
  _btn_prev = down;
}

// ---------------- button discovery (diagnostics) ----------------
static bool s_pinwatch = false;
static const uint8_t WATCH_PINS[] = { 0, 1, 7, 8, 12, 21, 45, 46 };
static int s_pinlast[sizeof(WATCH_PINS)];
static uint16_t s_xl_last = 0xFFFF;
static uint32_t s_pin_next = 0;

static void pinWatchPoll() {
  if (!s_pinwatch || millis() < s_pin_next) return;
  s_pin_next = millis() + 15;
  for (int i = 0; i < (int)sizeof(WATCH_PINS); i++) {
    int v = gpio_get_level((gpio_num_t)WATCH_PINS[i]);
    if (v != s_pinlast[i]) { Serial.printf("PIN gpio%d=%d\n", WATCH_PINS[i], v); s_pinlast[i] = v; }
  }
  Wire.beginTransmission(MAX_ADDR_XL9555); Wire.write(0x00);
  if (Wire.endTransmission(false) == 0 && Wire.requestFrom((uint8_t)MAX_ADDR_XL9555, (uint8_t)2) == 2) {
    uint16_t v = Wire.read(); v |= (uint16_t)Wire.read() << 8;
    if (v != s_xl_last) { Serial.printf("PIN xl9555_in=%04X\n", v); s_xl_last = v; }
  }
}

// ---------------- serial test console ----------------
// capacitive keys under the screen (ids 0..2) -> navigation
static const int TOUCH_KEY_MAP[3] = { MK_UP, MK_BACK, MK_DOWN };

int UITask::nextKey() {
  int k = keyboard.read();
  if (k != MK_NONE) return k;
  int t = touch.poll();
  if (t >= 0) {
    if (keyboard.log_raw) Serial.printf("TOUCHKEY id=%d\n", t);
    if (t < 3) return TOUCH_KEY_MAP[t];
    if (t == 16) return MK_TOP;       // hold heart
    if (t == 18) return MK_BOTTOM;    // hold plane
  }
  if (_inj_h != _inj_t) { k = _inj[_inj_h]; _inj_h = (_inj_h + 1) % 96; return k; }
  return MK_NONE;
}

void UITask::serialPoll() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { _ser[_ser_n] = 0; if (_ser_n) serialCommand(_ser); _ser_n = 0; }
    else if (_ser_n < (int)sizeof(_ser) - 1) _ser[_ser_n++] = c;
  }
}

void UITask::printStatus() {
  readBattery();
  Serial.printf("STATUS fw=%s screen=%d modal=%d\n", FIRMWARE_VERSION, _scr, _modal);
  Serial.printf(" xl9555=%d keyboard=%d gps_pow=%d antenna=%s\n", board.expanderOk(), keyboard.isReady(), _gps_pow, board.isExternalAntenna() ? "ext" : "int");
  Serial.printf(" batt=%umV %d%% (gauge says %d%%) %dmA usb=%d\n", _batt_mv, _batt_pct, board.getGaugeSoc(), _batt_ma, _usb);
  Serial.printf(" boot reason=%d crashes=%u  before-reboot: key=%u screen=%u modal=%u stage=%u  uptime=%us\n",
                bc_reason, (unsigned)bc_crashes, (unsigned)bc_prev_key, (unsigned)bc_prev_scr, (unsigned)bc_prev_modal,
                (unsigned)bc_prev_stage, (unsigned)(millis() / 1000));
  Serial.printf(" radio=%.3f/%.1f/SF%d/CR%d tx=%d noise=%d rx=%u tx=%u rxerr=%u\n", _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr,
                _prefs->tx_power_dbm, radio_driver.getNoiseFloor(), (unsigned)radio_driver.getPacketsRecv(),
                (unsigned)radio_driver.getPacketsSent(), (unsigned)radio_driver.getPacketsRecvErrors());
  Serial.printf(" name=%s contacts=%d msgs=%d unread=%d ble=%d conn=%d pin=%06u\n", _prefs->node_name, the_mesh.getNumContacts(),
                _store.count(), _store.totalUnread(), isBluetoothEnabled(), hasConnection(), (unsigned)the_mesh.getBLEPin());
  Serial.printf(" clock=%u valid=%d heap=%u psram=%u spiffs=%u/%u refreshes=%u\n", (unsigned)rtc_clock.getCurrentTime(), clockValid(),
                (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(), (unsigned)SPIFFS.usedBytes(), (unsigned)SPIFFS.totalBytes(),
                (unsigned)_disp->refreshCount());
  Serial.printf(" stack: loop task min free %u of %u bytes\n",
                (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)getArduinoLoopTaskStackSize());
  LocationProvider* lp = ((EnvironmentSensorManager*)_sensors)->getLocationProvider();
  if (lp) { char d[160]; lp->formatDiagnostics(d, sizeof(d)); Serial.printf(" gps: %s\n", d); }
  Serial.println(" end");
}

void UITask::serialCommand(char* line) {
  if (strcmp(line, "!status") == 0) { printStatus(); return; }
  if (strcmp(line, "!shot") == 0) {
    // dump the frame buffer: 240x320, 1bpp, 1 = white, rows of 30 bytes
    const uint8_t* b = _disp->buffer();
    Serial.println("SHOT 240 320");
    char hx[61];
    for (int r = 0; r < 320; r++) {
      for (int i = 0; i < 30; i++) sprintf(hx + i * 2, "%02X", b[r * 30 + i]);
      Serial.println(hx);
    }
    Serial.println("END");
    return;
  }
  if (strncmp(line, "!keys ", 6) == 0) {
    // plain chars are typed; escapes: \n enter, \b backspace, ~w ~s ~a ~d (alt nav), ~q back, ~$ menu, ~X (Alt+X)
    for (char* p = line + 6; *p; p++) {
      int k = *p;
      if (*p == '\\' && p[1]) { p++; k = *p == 'n' ? MK_ENTER : *p == 'b' ? MK_BS : *p == 's' ? ' ' : *p; }
      else if (*p == '~' && p[1]) {
        p++;
        switch (*p) {
          case 'w': k = MK_UP; break; case 's': k = MK_DOWN; break; case 'a': k = MK_LEFT; break; case 'd': k = MK_RIGHT; break;
          case 'q': k = MK_BACK; break; case '$': k = MK_DOLLAR; break; case 'm': k = MK_MIC; break;
          default: k = MK_ALT + tolower(*p); break;
        }
      }
      int n = (_inj_t + 1) % 96;
      if (n != _inj_h) { _inj[_inj_t] = k; _inj_t = n; }
    }
    Serial.println("OK");
    return;
  }
  // ---- import helpers (used by tools/import_json.py) ----
  // !addch <hex 32 or 64 chars> <name>
  if (strncmp(line, "!addch ", 7) == 0) {
    char* hex = line + 7;
    char* name = strchr(hex, ' ');
    if (!name) { Serial.println("ERR args"); return; }
    *name++ = 0;
    int klen = strlen(hex) / 2;
    ChannelDetails ch; memset(&ch, 0, sizeof(ch));
    if ((klen != 16 && klen != 32) || !mesh::Utils::fromHex(ch.channel.secret, klen, hex)) { Serial.println("ERR key"); return; }
    StrHelper::strncpy(ch.name, name, sizeof(ch.name));
    int slot = -1;
    for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
      ChannelDetails ex;
      if (!the_mesh.getChannel(i, ex)) continue;
      if (ex.name[0] && memcmp(ex.channel.secret, ch.channel.secret, 32) == 0) { Serial.println("OK exists"); return; }
      if (!ex.name[0] && slot < 0) slot = i;
    }
    if (slot < 0) { Serial.println("ERR full"); return; }
    the_mesh.setChannel(slot, ch);
    the_mesh.uiSaveChannels();
    _dirty = true;
    Serial.println("OK added");
    return;
  }
  // !addct <type> <hex64 pubkey> <lat> <lon> <name>   (added as a favourite)
  if (strncmp(line, "!addct ", 7) == 0) {
    int type = 0; char hex[70]; double lat = 0, lon = 0; int consumed = 0;
    if (sscanf(line + 7, "%d %66s %lf %lf %n", &type, hex, &lat, &lon, &consumed) < 4 || strlen(hex) != 64) { Serial.println("ERR args"); return; }
    const char* name = line + 7 + consumed;
    uint8_t pub[PUB_KEY_SIZE];
    if (!mesh::Utils::fromHex(pub, PUB_KEY_SIZE, hex)) { Serial.println("ERR key"); return; }
    ContactInfo* ex = the_mesh.lookupContactByPubKey(pub, PUB_KEY_SIZE);
    if (ex) {
      ex->flags |= 0x01;
      if (name[0]) StrHelper::strncpy(ex->name, name, sizeof(ex->name));
      the_mesh.uiSaveContactsSoon();
      Serial.println("OK updated");
      return;
    }
    ContactInfo c; memset(&c, 0, sizeof(c));
    memcpy(c.id.pub_key, pub, PUB_KEY_SIZE);
    StrHelper::strncpy(c.name, name, sizeof(c.name));
    c.type = type;
    c.flags = 0x01;                         // favourite
    c.out_path_len = OUT_PATH_UNKNOWN;      // learn the route on the mesh
    c.gps_lat = (int32_t)(lat * 1e6);
    c.gps_lon = (int32_t)(lon * 1e6);
    c.lastmod = rtc_clock.getCurrentTime();
    if (!the_mesh.addContact(c)) { Serial.println("ERR full"); return; }
    the_mesh.uiSaveContactsSoon();
    _dirty = true;
    Serial.println("OK added");
    return;
  }
  if (strcmp(line, "!pinwatch on") == 0) {
    static const uint8_t in_pins[] = { 1, 7, 8, 12, 21, 45, 46 };   // unused inputs (modem/IMU/GPS rails are off)
    for (uint8_t p : in_pins) pinMode(p, INPUT);
    for (int i = 0; i < (int)sizeof(WATCH_PINS); i++) s_pinlast[i] = -1;
    s_xl_last = 0xFFFF;
    s_pinwatch = true; Serial.println("OK"); return;
  }
  if (strcmp(line, "!pinwatch off") == 0) { s_pinwatch = false; Serial.println("OK"); return; }
  if (strcmp(line, "!keylog on") == 0) { keyboard.log_raw = touch.log_raw = true; Serial.printf("OK touch=%d\n", touch.isReady()); return; }
  if (strcmp(line, "!keylog off") == 0) { keyboard.log_raw = touch.log_raw = false; Serial.println("OK"); return; }
  if (strcmp(line, "!unlock") == 0) {           // test console: same as pressing the side button
    if (_scr == SCR_LOCK) { back(); applyLights(); _need_full = true; _last_input = millis(); }
    Serial.println("OK");
    return;
  }
  if (strncmp(line, "!settime ", 9) == 0) {      // !settime <unix epoch, UTC>  (e.g. from a PC)
    uint32_t t = strtoul(line + 9, nullptr, 10);
    if (t > 1700000000UL) { rtc_clock.setCurrentTime(t); _dirty = true; Serial.printf("OK clock=%u\n", (unsigned)rtc_clock.getCurrentTime()); }
    else Serial.println("ERR time");
    return;
  }
  if (strcmp(line, "!i2cscan") == 0) {
    Serial.print("I2C:");
    for (uint8_t a = 1; a < 127; a++) { Wire.beginTransmission(a); if (Wire.endTransmission() == 0) Serial.printf(" %02X", a); }
    Serial.println();
    return;
  }
  if (strcmp(line, "!buzz") == 0) { board.buzz(14); Serial.println("OK"); return; }
  Serial.println("? commands: !status !shot !keys <text> !buzz");
}

void UITask::loop() {
  uint32_t now = millis();

  serialPoll();
  pinWatchPoll();

  // keyboard
  int k;
  while ((k = nextKey()) != MK_NONE) {
    _last_input = now;
    if (k == MK_ALT + 'e') k = MK_BOTTOM;   // end
    if (k == MK_ALT + 't') k = MK_TOP;      // top
    bc_key = k; bc_scr = _scr; bc_modal = _modal; bcMark(BC_KEY);
    if (_scr == SCR_LOCK) continue;   // locked: only the side (BOOT) button unlocks
    if (k == MK_ALT + 'l') { go(SCR_LOCK); applyLights(); _need_full = true; continue; }
    if (keyboard.log_raw) Serial.printf("KEY decoded=%d '%c'\n", k, (k >= 32 && k < 127) ? k : '?');
    if (k == MK_MIC) {
      _cfg.kb_light = !_cfg.kb_light; applyLights(); saveCfg();
      toast("Keyboard light: %s", _cfg.kb_light ? "On" : "Off"); continue;
    }
    if (k == MK_DOLLAR || k == MK_ALT + 'b') {
      _cfg.frontlight = (_cfg.frontlight + 1) & 3; applyLights(); saveCfg();
      toast("Frontlight: %s", LIGHT_NAMES[_cfg.frontlight]); continue;
    }
    if (k == MK_ALT + 'h') { _modal = MODAL_NONE; _sp = 0; _scr = SCR_HOME; _dirty = true; continue; }
    if (k == MK_ALT + 'r') { _need_full = true; _dirty = true; continue; }   // force clean refresh
    if (_modal != MODAL_NONE) { modalKey(k); continue; }
    switch (_scr) {
      case SCR_HOME: keyHome(k); break;
      case SCR_CONVS: keyConvs(k); break;
      case SCR_CHAT: keyChat(k); break;
      case SCR_CHANNELS: keyChannels(k); break;
      case SCR_CONTACTS: keyContacts(k); break;
      case SCR_CONTACT_MENU: keyContactMenu(k); break;
      case SCR_INFO: keyInfo(k); break;
      case SCR_ADMIN: keyAdmin(k); break;
      case SCR_HEARD: keyHeard(k); break;
      case SCR_GPS: keyGps(k); break;
      case SCR_SETTINGS: keySettings(k); break;
      case SCR_DEVICE: keyDevice(k); break;
      default: break;
    }
    _dirty = true;
  }

  if (keyboard.modsChanged()) _dirty = true;   // show ALT / SYM / CAPS indicator right away
  handleButton();
  retryLoop();

  // request timeouts
  if (_pend_kind && (int32_t)(now - _pend_deadline) > 0) {
    admLog(_pend_kind == 1 ? "Login timed out (no reply)" : "Status timed out (no reply)");
    _pend_kind = 0;
  }

  // periodic
  if ((int32_t)(now - _batt_next) > 0) {
    int old = _batt_pct;
    bool was_usb = _usb;
    readBattery();
    _batt_next = now + 30000;
    if (_usb && !was_usb && _has_slept) usbPluggedAfterSleep();
    if (old != _batt_pct) _dirty = true;
  }
  uint32_t minute = rtc_clock.getCurrentTime() / 60;
  if (minute != _last_minute) { _last_minute = minute; _dirty = true; }
  if (_toast[0] && (int32_t)(now - _toast_until) > 0) { _toast[0] = 0; _dirty = true; }
  gpsPowerSync();

  // auto-lock
  if (_cfg.lock_secs && _scr != SCR_LOCK && now - _last_input > (uint32_t)_cfg.lock_secs * 1000UL) {
    go(SCR_LOCK);          // an open dialog (and what was typed) is kept for after unlocking
    applyLights();
    _need_full = true;
  }

  bcMark(BC_SAVE);
  _store.saveIfNeeded(_scr == SCR_LOCK);

  if (_dirty) {
    _dirty = false;
    bc_scr = _scr; bc_modal = _modal; bcMark(BC_RENDER);
    render();
  }
  bcMark(BC_IDLE);

  // ---- power saving: light sleep between radio/keyboard events ----
  // (no light sleep while the frontlight is on: its PWM clock stops in sleep -> flicker)
  bool can_sleep = !_usb && !isBluetoothEnabled() && !_gps_pow && !_dirty && board.getFrontlight() == 0
                   && !keyboard.keyHeld() && !the_mesh.hasPendingWork() && _pend_kind == 0
                   && now - _last_input > 3000 && now - _boot_ms > 15000;
  if (can_sleep) {
    for (int i = _store.count() - 1, n = 0; i >= 0 && n < 20; i--, n++)
      if (_store.at(i)->status == MS_SENDING) { can_sleep = false; break; }
  }
  if (can_sleep && board.isUsbPowered()) {   // just plugged in
    _usb = true; can_sleep = false; _dirty = true;
    if (_has_slept) usbPluggedAfterSleep();
  }
  if (can_sleep) {
    _has_slept = true;
    uint32_t secs = rtc_clock.getCurrentTime() % 60;
    uint32_t ms = (60 - secs) * 1000;            // wake for the next clock minute
    if (_toast[0]) ms = 500;
    bcMark(BC_SLEEP);
    board.lightSleepUntilEvent(min(ms, (uint32_t)30000));
    bcMark(BC_IDLE);
  }
}

// ======================================================================
//  drawing primitives
// ======================================================================

static const GFXfont* s_font = nullptr;
static int s_asc = 0;

void UITask::font(int f) {
  switch (f) {
    case 0: s_font = nullptr; s_asc = 0; break;
    case 1: s_font = &FreeSans9pt7b; s_asc = 13; break;
    case 2: s_font = &FreeSansBold9pt7b; s_asc = 13; break;
    case 3: s_font = &FreeSansBold12pt7b; s_asc = 17; break;
    case 4: s_font = &FreeSansBold18pt7b; s_asc = 25; break;
    case 5: s_font = &FreeSansBold24pt7b; s_asc = 34; break;
  }
  g().setFont(s_font);
  g().setTextSize(1);
}

static int charW(char c) {
  if (!s_font) return 6;
  if (c < s_font->first || c > s_font->last) return 0;
  return s_font->glyph[c - s_font->first].xAdvance;
}

int UITask::textW(const char* s) {
  int w = 0;
  while (*s) w += charW(*s++);
  return w;
}

// draw text at top-left (x, y) regardless of font type
static void putText(Adafruit_GFX& gx, int x, int y, const char* s) {
  gx.setCursor(x, y + s_asc);
  gx.print(s);
}

// dashed underline under words the spell checker doesn't know (draw-time only)
static void underlineMisspelled(Adafruit_GFX& gx, const char* buf, int line_off, int take, int x, int base_y) {
  int px = x;
  int i = 0;
  const char* line = buf + line_off;
  while (i < take) {
    if (isalpha((unsigned char)line[i]) || (line[i] == '\'' && i > 0 && isalpha((unsigned char)line[i - 1]))) {
      int s = i, sx = px;
      while (i < take && (isalpha((unsigned char)line[i]) || line[i] == '\'')) { px += charW(line[i]); i++; }
      int len = i - s;
      char after = line[i];               // may be beyond 'take' (next line) - that's fine, it's still buf
      bool typing = (after == '|' && line[i + 1] == 0);   // word under the cursor: still being typed
      if (!typing && spell.needsCheck(buf, line_off + s, len) && !spell.known(line + s, len)) {
        for (int ux = sx; ux < px - 1; ux += 3) {        // dashed, 2 px thick for e-paper visibility
          gx.drawFastHLine(ux, base_y + 2, 2, GxEPD_BLACK);
          gx.drawFastHLine(ux, base_y + 3, 2, GxEPD_BLACK);
        }
      }
    } else {
      px += charW(line[i]);
      i++;
    }
  }
}

int UITask::wrapText(const char* text, int x, int y, int w, int line_h, bool draw, int max_lines, bool spellcheck) {
  char buf[MSG_TEXT_MAX + 8];
  toAscii(buf, text, sizeof(buf));
  int lines = 0;
  const char* p = buf;
  while (*p && lines < max_lines) {
    // find how many chars fit on this line, prefer breaking at a space
    int width = 0, n = 0, last_space = -1;
    while (p[n]) {
      int cw = charW(p[n]);
      if (width + cw > w) break;
      if (p[n] == ' ') last_space = n;
      width += cw; n++;
    }
    int take = n;
    if (p[n] && last_space > 0) take = last_space;
    if (take == 0) take = max(1, n);
    if (draw) {
      char line[MSG_TEXT_MAX + 8];
      memcpy(line, p, take); line[take] = 0;
      putText(g(), x, y + lines * line_h, line);
      if (spellcheck && !_cfg.spell_off) underlineMisspelled(g(), buf, p - buf, take, x, y + lines * line_h + s_asc);
    }
    lines++;
    p += take;
    while (*p == ' ') p++;
  }
  return max(lines, 1);
}

void UITask::header(const char* title) {
  g().fillRect(0, 0, W, HDR_H, BLACK);
  g().setTextColor(WHITE);
  font(0);
  // right side: time, BT, battery
  char right[40] = "";
  char tm[16];
  if (clockValid()) { uint32_t t = localNow(); sprintf(tm, "%02u:%02u", (unsigned)((t / 3600) % 24), (unsigned)((t / 60) % 60)); }
  else strcpy(tm, "--:--");
  int unread = _store.totalUnread();
  char un[8] = ""; if (unread && _scr != SCR_CONVS) sprintf(un, "*%d ", unread);
  char bt[6] = ""; if (isBluetoothEnabled()) strcpy(bt, hasConnection() ? "BT+ " : "BT ");
  char bat[10];
  if (_batt_pct >= 0) sprintf(bat, "%d%%%s", _batt_pct, _usb ? "+" : "");   // '+' = on USB / charging
  else strcpy(bat, _usb ? "USB" : "?");
  snprintf(right, sizeof(right), "%s%s%s %s", un, bt, tm, bat);
  int rw = textW(right);
  putText(g(), W - rw - 3, 5, right);
  // title (truncate to fit)
  char t[40]; toAscii(t, title, sizeof(t));
  int maxw = W - rw - 12;
  while (textW(t) > maxw && strlen(t) > 1) t[strlen(t) - 1] = 0;
  putText(g(), 3, 5, t);
  g().setTextColor(BLACK);
  if (_toast[0]) {
    font(0);
    char tt[64]; toAscii(tt, _toast, sizeof(tt));
    int y = HDR_H + 1;
    g().fillRect(0, y, W, 13, BLACK);
    g().setTextColor(WHITE);
    while (textW(tt) > W - 6 && strlen(tt) > 1) tt[strlen(tt) - 1] = 0;
    putText(g(), 3, y + 3, tt);
    g().setTextColor(BLACK);
  }
}

void UITask::footer(const char* hint) {
  font(0);
  g().drawFastHLine(0, H - FTR_H, W, BLACK);
  putText(g(), 3, H - FTR_H + 3, hint);
  // modifier indicators
  char mods[12] = "";
  if (keyboard.capsOn()) strcat(mods, "CAPS ");
  else if (keyboard.shiftOn()) strcat(mods, "Sh ");
  if (keyboard.symLock()) strcat(mods, "SYM");
  else if (keyboard.symOn()) strcat(mods, "sym");
  if (keyboard.altOn()) strcat(mods, "ALT");
  if (mods[0]) {
    int w = textW(mods);
    g().fillRect(W - w - 6, H - FTR_H + 1, w + 6, FTR_H - 1, BLACK);
    g().setTextColor(WHITE);
    putText(g(), W - w - 3, H - FTR_H + 3, mods);
    g().setTextColor(BLACK);
  }
}

void UITask::listRow(int y, int h, bool sel) {
  if (sel) { g().fillRect(0, y, W, h, BLACK); g().setTextColor(WHITE); }
  else g().setTextColor(BLACK);
}

void UITask::drawList(int scr, int n, int row_h, void (UITask::*row)(int idx, int y, bool sel)) {
  int y0 = HDR_H + (_toast[0] ? 14 : 2);
  int rows = (H - FTR_H - y0) / row_h;
  int& sel = _sel[scr];
  int& top = _top[scr];
  if (n == 0) { font(1); g().setTextColor(BLACK); putText(g(), 10, y0 + 10, "(empty)"); return; }
  sel = constrain(sel, 0, n - 1);
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;
  top = constrain(top, 0, max(0, n - rows));
  for (int i = top; i < n && i < top + rows; i++) {
    int y = y0 + (i - top) * row_h;
    listRow(y, row_h, i == sel);
    (this->*row)(i, y, i == sel);
    g().setTextColor(BLACK);
  }
  // scrollbar
  if (n > rows) {
    int bh = max(10, (H - FTR_H - y0) * rows / n);
    int by = y0 + (H - FTR_H - y0 - bh) * top / max(1, n - rows);
    g().fillRect(W - 3, by, 3, bh, BLACK);
  }
}

static bool listNav(int k, int& sel, int n, int page = 6) {
  if (n <= 0) return false;
  if (k == 'w' || k == MK_UP) { sel = (sel + n - 1) % n; return true; }
  if (k == 's' || k == MK_DOWN) { sel = (sel + 1) % n; return true; }
  if (k == MK_TOP) { sel = 0; return true; }
  if (k == MK_BOTTOM) { sel = n - 1; return true; }
  if (k == 'a' || k == MK_LEFT) { sel = max(0, sel - page); return true; }
  if (k == 'd' || k == MK_RIGHT) { sel = min(n - 1, sel + page); return true; }
  return false;
}

static bool isBack(int k) { return k == 'q' || k == MK_BACK || k == MK_BS; }

// ======================================================================
//  render dispatch
// ======================================================================

void UITask::render() {
  if (!_disp) return;
  if (_need_full) { _disp->forceFullRefresh(); _need_full = false; }
  _disp->startFrame();
  g().setTextColor(BLACK);
  switch (_scr) {
    case SCR_HOME: drawHome(); break;
    case SCR_CONVS: drawConvs(); break;
    case SCR_CHAT: drawChat(); break;
    case SCR_CHANNELS: drawChannels(); break;
    case SCR_CONTACTS: drawContacts(); break;
    case SCR_CONTACT_MENU: drawContactMenu(); break;
    case SCR_INFO: drawInfo(); break;
    case SCR_ADMIN: drawAdmin(); break;
    case SCR_HEARD: drawHeard(); break;
    case SCR_GPS: drawGps(); break;
    case SCR_SETTINGS: drawSettings(); break;
    case SCR_DEVICE: drawDevice(); break;
    case SCR_LOCK: drawLock(); break;
  }
  if (_modal != MODAL_NONE && _scr != SCR_LOCK) drawModal();
  _disp->commit();
}

// ======================================================================
//  HOME
// ======================================================================

struct HomeItem { char key; const char* label; };
static const HomeItem HOME_ITEMS[] = {
  { 'm', "Messages" }, { 'c', "Channels" }, { 'k', "Contacts" }, { 'r', "Repeaters & Rooms" },
  { 'h', "Heard nearby" }, { 'z', "Send advert" }, { 'f', "Send flood advert" },
  { 'g', "GPS & location" }, { 'e', "Settings" }, { 'i', "Device info" }, { 'l', "Lock screen" },
};
#define NUM_HOME (int)(sizeof(HOME_ITEMS) / sizeof(HOME_ITEMS[0]))

void UITask::rowHome(int i, int y, bool sel) {
  font(2);
  char lbl[48];
  strcpy(lbl, HOME_ITEMS[i].label);
  if (i == 0) { int u = _store.totalUnread(); if (u) sprintf(lbl, "Messages  (%d new)", u); }
  if (i == 2) sprintf(lbl, "Contacts  (%d)", the_mesh.getNumContacts());
  putText(g(), 28, y + 4, lbl);
  font(0);
  char hk[4] = { (char)toupper(HOME_ITEMS[i].key), 0 };
  g().drawRect(6, y + 5, 13, 13, sel ? WHITE : BLACK);
  putText(g(), 10, y + 8, hk);
}

void UITask::drawHome() {
  char t[40]; toAscii(t, _prefs->node_name, sizeof(t));
  header(t);
  drawList(SCR_HOME, NUM_HOME, 24, &UITask::rowHome);
  footer("W/S move  Ent open  letter=shortcut");
}

void UITask::keyHome(int k) {
  int& sel = _sel[SCR_HOME];
  if (listNav(k, sel, NUM_HOME)) return;
  int pick = -1;
  if (k == MK_ENTER || k == ' ') pick = sel;
  else if (k >= 'a' && k <= 'z') for (int i = 0; i < NUM_HOME; i++) if (HOME_ITEMS[i].key == k) { pick = i; sel = i; }
  switch (pick) {
    case 0: go(SCR_CONVS); break;
    case 1: go(SCR_CHANNELS); break;
    case 2: _ct_filter = 0; _find[0] = 0; buildContactList(); go(SCR_CONTACTS); break;
    case 3: _ct_filter = 2; _find[0] = 0; buildContactList(); go(SCR_CONTACTS); break;
    case 4: _nheard = the_mesh.getRecentlyHeard(_heard, 16); go(SCR_HEARD); break;
    case 5: doAdvert(false); break;
    case 6: doAdvert(true); break;
    case 7: go(SCR_GPS); break;
    case 8: go(SCR_SETTINGS); break;
    case 9: go(SCR_DEVICE); break;
    case 10: go(SCR_LOCK); applyLights(); _need_full = true; break;
  }
}

// ======================================================================
//  CONVERSATIONS
// ======================================================================

void UITask::rowConv(int i, int y, bool sel) {
  ConvEntry& c = _convs[i];
  char name[40]; convName(c.key, name, sizeof(name));
  if (c.key.kind == CONV_CHANNEL && name[0] != '#') { char t[42]; snprintf(t, sizeof(t), "#%s", name); strcpy(name, t); }
  font(c.unread ? 2 : 1);
  char tm[16]; fmtTime(c.last, tm);
  font(0); int tw = textW(tm);
  putText(g(), W - tw - 6, y + 4, tm);
  font(c.unread ? 2 : 1);
  if (c.unread) { char t[48]; snprintf(t, sizeof(t), "%s (%d)", name, c.unread); strcpy(name, t); }
  while (textW(name) > W - tw - 14 && strlen(name) > 1) name[strlen(name) - 1] = 0;
  putText(g(), 4, y + 2, name);
  // preview
  StoredMsg* m = _store.at(c.last_idx);
  if (m) {
    font(0);
    char pv[64], raw[200];
    if (m->flags & MF_OUT) snprintf(raw, sizeof(raw), "You: %s", m->text);
    else if (c.key.kind == CONV_CHANNEL || (m->flags & MF_ROOM)) snprintf(raw, sizeof(raw), "%s: %s", m->author, m->text);
    else snprintf(raw, sizeof(raw), "%s", m->text);
    toAscii(pv, raw, sizeof(pv));
    while (textW(pv) > W - 12 && strlen(pv) > 1) pv[strlen(pv) - 1] = 0;
    putText(g(), 4, y + 21, pv);
  }
  if (!sel) g().drawFastHLine(4, y + 32, W - 8, BLACK);
}

void UITask::drawConvs() {
  buildConvs();
  header("Messages");
  drawList(SCR_CONVS, _nconv, 34, &UITask::rowConv);
  footer("Ent open  N new  X delete  Q back");
}

void UITask::keyConvs(int k) {
  int& sel = _sel[SCR_CONVS];
  buildConvs();
  if (listNav(k, sel, _nconv, 4)) return;
  if (isBack(k)) { back(); return; }
  if ((k == MK_ENTER || k == ' ') && _nconv) { openChat(_convs[sel].key); return; }
  if (k == 'n') { _pick_for_msg = true; _ct_filter = 1; _find[0] = 0; buildContactList(); go(SCR_CONTACTS); return; }
  if (k == 'x' && _nconv) { _chat = _convs[sel].key; openConfirm(P_DEL_CONV, "Delete this conversation's history?"); }
}

// ======================================================================
//  CHAT
// ======================================================================

void UITask::drawChat() {
  char name[40]; convName(_chat, name, sizeof(name));
  char title[44];
  if (_chat.kind == CONV_CHANNEL && name[0] != '#') snprintf(title, sizeof(title), "#%s", name); else strcpy(title, name);
  header(title);

  // collect message indexes for this conversation
  static int idx[MSG_STORE_SIZE];
  int n = 0;
  for (int i = 0; i < _store.count(); i++) {
    StoredMsg* m = _store.at(i);
    if (m->conv == _chat && !(m->flags & MF_CLI)) idx[n++] = i;
  }
  _scroll = constrain(_scroll, 0, max(0, n - 1));

  // input box
  font(1);
  const int LH = 18;
  int in_lines = _input[0] ? min(4, wrapText(_input, 0, 0, W - 12, LH, false)) : 1;
  int in_h = in_lines * LH + 8;
  int in_y = H - FTR_H - in_h;
  g().drawFastHLine(0, in_y, W, BLACK);
  g().drawFastHLine(0, in_y + 1, W, BLACK);
  int prefix = strlen(_prefs->node_name) + 2;
  int maxlen = (_chat.kind == CONV_CHANNEL) ? (MAX_TEXT_LEN - prefix) : MAX_TEXT_LEN;
  if (_input[0]) {
    char tmp[MSG_TEXT_MAX + 2];
    snprintf(tmp, sizeof(tmp), "%s|", _input);
    // show the end of long input
    int lines = wrapText(tmp, 0, 0, W - 12, LH, false);
    if (lines > 4) {
      // drop leading text until it fits in 4 lines
      const char* p = tmp;
      while (*p && wrapText(p, 0, 0, W - 12, LH, false) > 4) p++;
      wrapText(p, 4, in_y + 4, W - 12, LH, true, 4, true);
    } else {
      wrapText(tmp, 4, in_y + 4, W - 12, LH, true, 4, true);
    }
  } else {
    putText(g(), 4, in_y + 4, "|");
    font(0);
    putText(g(), 14, in_y + 9, "Type a message...");
  }
  font(0);
  char cnt[12]; sprintf(cnt, "%d", maxlen - (int)strlen(_input));
  putText(g(), W - textW(cnt) - 3, in_y - 10, cnt);

  // messages, newest at the bottom
  int top_limit = HDR_H + (_toast[0] ? 16 : 3);
  int y = in_y - 12;
  int shown = 0;
  if (_jump_top) {
    // "jump to oldest": choose the scroll that fills the screen starting at message 0
    _jump_top = false;
    int avail = y - top_limit, acc = 0, fit = 0;
    for (int j = 0; j < n; j++) {
      StoredMsg* m = _store.at(idx[j]);
      int w = W - ((m->flags & MF_OUT) ? 36 : 4) - 6;
      font(1);
      int h = 11 + wrapText(m->text, 0, 0, w, LH, false) * LH + 4 + 2;
      if (m == _new_marker && m->ts == _new_marker_ts) h += 15;
      if (acc + h > avail) break;
      acc += h; fit++;
    }
    _scroll = max(0, n - max(fit, 1));
  }
  for (int j = n - 1 - _scroll; j >= 0; j--) {
    StoredMsg* m = _store.at(idx[j]);
    bool out = m->flags & MF_OUT;
    int x = out ? 36 : 4;
    int w = W - x - 6;
    font(1);
    int lines = wrapText(m->text, 0, 0, w, LH, false);
    int h = 11 + lines * LH + 4;
    bool divider = (m == _new_marker && m->ts == _new_marker_ts);
    const int DIV_H = 15;
    if (y - h - (divider ? DIV_H : 0) < top_limit) break;
    int ty = y - h;
    if (divider) {
      // ---------- New Messages ----------
      int dy = ty - DIV_H + 2;
      font(0);
      const char* lbl = "New Messages";
      int tw = textW(lbl);
      int cx = W / 2;
      g().fillRect(4, dy + 5, cx - tw / 2 - 10, 2, BLACK);
      g().fillRect(cx + tw / 2 + 6, dy + 5, W - 4 - (cx + tw / 2 + 6), 2, BLACK);
      g().fillRect(cx - tw / 2 - 4, dy, tw + 8, 12, BLACK);
      g().setTextColor(WHITE); putText(g(), cx - tw / 2, dy + 2, lbl); g().setTextColor(BLACK);
    }
    // meta line
    font(0);
    char meta[64], tm[16], who[MSG_AUTHOR_MAX + 1];
    fmtTime(m->sender_ts && !out && m->sender_ts > 1600000000UL ? m->sender_ts : m->ts, tm);
    toAscii(who, m->author, sizeof(who));
    if (out) {
      const char* st = m->status == MS_DELIVERED ? " ok" : m->status == MS_SENDING ? " ..." :
                       m->status == MS_FAILED ? " FAILED" : m->status == MS_SENT ? " sent" : "";
      char att[8] = ""; if (m->status == MS_SENDING && m->attempts) sprintf(att, " (%d)", m->attempts + 1);
      snprintf(meta, sizeof(meta), "%s%s%s", tm, st, att);
      putText(g(), W - textW(meta) - 4, ty, meta);
      g().drawFastVLine(x - 6, ty + 10, lines * LH + 2, BLACK);
      g().drawFastVLine(x - 5, ty + 10, lines * LH + 2, BLACK);
    } else {
      char hops[12] = "";
      if (m->hops == 0xFF) strcpy(hops, " direct");
      else if (m->hops > 0) sprintf(hops, " %dhop", m->hops);
      if (_chat.kind == CONV_CHANNEL || (m->flags & MF_ROOM)) snprintf(meta, sizeof(meta), "%s  %s%s", who, tm, hops);
      else snprintf(meta, sizeof(meta), "%s%s", tm, hops);
      putText(g(), x, ty, meta);
    }
    font(1);
    wrapText(m->text, x, ty + 11, w, LH, true);
    y = ty - 2 - (divider ? DIV_H : 0);
    shown++;
  }
  if (n == 0) { font(1); putText(g(), 10, HDR_H + 30, "No messages yet."); }
  if (_scroll > 0) {
    font(0);
    char s[24]; sprintf(s, "v %d newer v", _scroll);
    int sw = textW(s);
    g().fillRect(W / 2 - sw / 2 - 3, in_y - 11, sw + 6, 10, BLACK);
    g().setTextColor(WHITE); putText(g(), W / 2 - sw / 2, in_y - 10, s); g().setTextColor(BLACK);
  }
  footer(_chat.kind == CONV_DM ? "Ent send  Alt+W/S scroll  Alt+M menu" : "Ent send  Alt+W/S scroll  Bksp back");
}

void UITask::keyChat(int k) {
  int len = strlen(_input);
  int prefix = strlen(_prefs->node_name) + 2;
  int maxlen = (_chat.kind == CONV_CHANNEL) ? (MAX_TEXT_LEN - prefix) : MAX_TEXT_LEN;
  maxlen = min(maxlen, MSG_TEXT_MAX - 1);
  if (k == MK_BACK) { back(); return; }
  if (k == MK_UP) { _scroll++; return; }
  if (k == MK_DOWN) { if (_scroll > 0) _scroll--; return; }
  if (k == MK_BOTTOM) { _scroll = 0; return; }
  if (k == MK_TOP) { _jump_top = true; return; }   // resolved in drawChat (fills screen from the oldest)
  if (k == MK_LEFT) { _scroll += 5; return; }
  if (k == MK_RIGHT) { _scroll = max(0, _scroll - 5); return; }
  if (k == MK_BS) { if (len) _input[len - 1] = 0; else back(); return; }
  if (k == MK_ENTER) { if (_input[0]) sendChat(); else _scroll = 0; return; }   // empty box: jump to newest
  if (k == MK_ALT + 'k') {        // keep: add the last underlined word to the personal dictionary
    int i = len;
    while (i > 0) {
      while (i > 0 && !isalpha((unsigned char)_input[i - 1])) i--;
      int e = i;
      while (i > 0 && (isalpha((unsigned char)_input[i - 1]) || _input[i - 1] == '\'')) i--;
      int wl = e - i;
      if (wl > 0 && spell.needsCheck(_input, i, wl) && !spell.known(_input + i, wl)) {
        char wd[SpellCheck::WORD_MAX]; int n = min(wl, SpellCheck::WORD_MAX - 1);
        memcpy(wd, _input + i, n); wd[n] = 0;
        toast(spell.addUserWord(_input + i, wl) ? "Added '%s' to dictionary" : "Dictionary full", wd);
        return;
      }
    }
    toast("No underlined words");
    return;
  }
  if (k == MK_ALT + 'm') {
    if (_chat.kind == CONV_DM) {
      ContactInfo* c = the_mesh.lookupContactByPubKey(_chat.id, 6);
      if (c) { memcpy(_cur_pub, c->id.pub_key, PUB_KEY_SIZE); _sel[SCR_CONTACT_MENU] = 0; go(SCR_CONTACT_MENU); }
    }
    return;
  }
  if (k >= 32 && k < 127 && len < maxlen) { _input[len] = (char)k; _input[len + 1] = 0; }
}

// ======================================================================
//  CHANNELS
// ======================================================================

static int s_chan_idx[MAX_GROUP_CHANNELS];
static int s_nchan = 0;

static void buildChannels() {
  s_nchan = 0;
  for (int i = 0; i < MAX_GROUP_CHANNELS; i++) {
    ChannelDetails ch;
    if (the_mesh.getChannel(i, ch) && ch.name[0]) s_chan_idx[s_nchan++] = i;
  }
}

void UITask::rowChannel(int i, int y, bool sel) {
  ChannelDetails ch; the_mesh.getChannel(s_chan_idx[i], ch);
  ConvKey k = keyForChannel(s_chan_idx[i]);
  int u = _store.unreadFor(k);
  font(u ? 2 : 1);
  char n[40], t[48]; toAscii(n, ch.name, sizeof(n));
  if (u) snprintf(t, sizeof(t), "%s%s  (%d)", n[0] == '#' ? "" : "", n, u); else strcpy(t, n);
  putText(g(), 6, y + 3, t);
}

void UITask::drawChannels() {
  buildChannels();
  header("Channels");
  drawList(SCR_CHANNELS, s_nchan, 24, &UITask::rowChannel);
  footer("Ent open  N new  I key  X delete  Q back");
}

void UITask::keyChannels(int k) {
  buildChannels();
  int& sel = _sel[SCR_CHANNELS];
  if (listNav(k, sel, s_nchan)) return;
  if (isBack(k)) { back(); return; }
  if ((k == MK_ENTER || k == ' ') && s_nchan) { openChat(keyForChannel(s_chan_idx[sel])); return; }
  if (k == 'n') { openInput(P_NEW_CHANNEL, "#hashtag, name, or 'public'", "#", 30); return; }
  if (k == 'x' && s_nchan) {
    ChannelDetails ch; the_mesh.getChannel(s_chan_idx[sel], ch);
    char t[80]; char n[32]; toAscii(n, ch.name, sizeof(n));
    snprintf(t, sizeof(t), "Delete channel '%s'?", n);
    openConfirm(P_DEL_CHANNEL, t);
    return;
  }
  if (k == 'i' && s_nchan) {
    ChannelDetails ch; the_mesh.getChannel(s_chan_idx[sel], ch);
    static const uint8_t zero[16] = { 0 };
    int klen = memcmp(&ch.channel.secret[16], zero, 16) == 0 ? 16 : 32;
    unsigned char b64[64];
    encode_base64(ch.channel.secret, klen, b64);
    char n[32]; toAscii(n, ch.name, sizeof(n));
    snprintf(_m_text, sizeof(_m_text), "%s\nKey (base64):\n%s", n, (char*)b64);
    _modal = MODAL_CONFIRM; _purpose = P_NONE;
  }
}

// ======================================================================
//  CONTACTS
// ======================================================================

void UITask::rowContact(int i, int y, bool sel) {
  ContactInfo c;
  the_mesh.getContactByIdx(_list_idx[i], c);
  const char* tag = c.type == ADV_TYPE_REPEATER ? "R" : c.type == ADV_TYPE_ROOM ? "Rm" : c.type == ADV_TYPE_SENSOR ? "S" : "";
  font(0);
  char right[24], age[8], hops[8];
  fmtAge(c.lastmod, age);
  if (c.out_path_len == OUT_PATH_UNKNOWN) strcpy(hops, "fl");
  else sprintf(hops, "%dh", c.out_path_len & 63);
  snprintf(right, sizeof(right), "%s %s", hops, age);
  int rw = textW(right);
  putText(g(), W - rw - 6, y + 7, right);
  int x = 4;
  if (tag[0]) {
    int tw = textW(tag) + 4;
    g().drawRect(x, y + 4, tw, 13, sel ? WHITE : BLACK);
    putText(g(), x + 2, y + 7, tag);
    x += tw + 4;
  }
  if (c.isFav()) { putText(g(), x, y + 7, "*"); x += 8; }
  font(1);
  char n[40]; toAscii(n, c.name, sizeof(n));
  while (textW(n) > W - rw - x - 12 && strlen(n) > 1) n[strlen(n) - 1] = 0;
  putText(g(), x, y + 3, n);
}

void UITask::drawContacts() {
  char t[48];
  if (_pick_for_msg) snprintf(t, sizeof(t), "Message who?");
  else snprintf(t, sizeof(t), "Contacts: %s%s%s", FILTER_NAMES[_ct_filter], _find[0] ? " /" : "", _find);
  header(t);
  drawList(SCR_CONTACTS, _list_n, 22, &UITask::rowContact);
  footer("Ent open  F find  Tab:Sp filter  Q back");
}

void UITask::keyContacts(int k) {
  int& sel = _sel[SCR_CONTACTS];
  if (k == 'w' || k == 's' || k == MK_UP || k == MK_DOWN || k == 'a' || k == 'd' || k == MK_LEFT || k == MK_RIGHT || k == MK_TOP || k == MK_BOTTOM) {
    listNav(k, sel, _list_n, 8);
    return;
  }
  if (isBack(k)) {
    if (_find[0]) { _find[0] = 0; buildContactList(); return; }
    _pick_for_msg = false; back(); return;
  }
  if (k == ' ') { _ct_filter = (_ct_filter + 1) % 5; _sel[SCR_CONTACTS] = 0; buildContactList(); return; }
  if (k == 'f') { openInput(P_FIND, "Find contact", _find, 20); return; }
  if (k == MK_ENTER && _list_n) {
    ContactInfo c; the_mesh.getContactByIdx(_list_idx[sel], c);
    memcpy(_cur_pub, c.id.pub_key, PUB_KEY_SIZE);
    if (_pick_for_msg) {
      _pick_for_msg = false;
      back();                          // leave the picker
      openChat(keyForContact(c));
      return;
    }
    _sel[SCR_CONTACT_MENU] = 0;
    go(SCR_CONTACT_MENU);
  }
}

// ======================================================================
//  CONTACT MENU / INFO
// ======================================================================

enum { CM_MESSAGE = 1, CM_ADMIN, CM_LOGIN, CM_INFO, CM_RESET_PATH, CM_SHARE, CM_FAV, CM_DELETE, CM_ROOM_CHAT };

int UITask::contactMenuItems(const ContactInfo* c, uint8_t* out) {
  int n = 0;
  if (!c) return 0;
  if (c->type == ADV_TYPE_CHAT) out[n++] = CM_MESSAGE;
  if (c->type == ADV_TYPE_ROOM) { out[n++] = CM_ROOM_CHAT; out[n++] = CM_LOGIN; }
  if (c->type == ADV_TYPE_REPEATER || c->type == ADV_TYPE_ROOM || c->type == ADV_TYPE_SENSOR) out[n++] = CM_ADMIN;
  out[n++] = CM_INFO;
  out[n++] = CM_RESET_PATH;
  out[n++] = CM_SHARE;
  out[n++] = CM_FAV;
  out[n++] = CM_DELETE;
  return n;
}

static const char* cmLabel(uint8_t id, const ContactInfo* c) {
  switch (id) {
    case CM_MESSAGE: return "Send message";
    case CM_ROOM_CHAT: return "Open room chat";
    case CM_LOGIN: return "Log in to room";
    case CM_ADMIN: return c && c->type == ADV_TYPE_ROOM ? "Room admin / status" : "Admin / status / CLI";
    case CM_INFO: return "Info & location";
    case CM_RESET_PATH: return "Reset path (re-flood)";
    case CM_SHARE: return "Share contact (zero hop)";
    case CM_FAV: return c && c->isFav() ? "Remove favourite" : "Mark favourite";
    case CM_DELETE: return "Delete contact";
  }
  return "?";
}

void UITask::rowContactMenu(int i, int y, bool sel) {
  uint8_t items[12]; ContactInfo* c = curContact();
  contactMenuItems(c, items);
  font(1);
  putText(g(), 10, y + 3, cmLabel(items[i], c));
}

void UITask::drawContactMenu() {
  ContactInfo* c = curContact();
  if (!c) { back(); header("Contact"); return; }
  char n[40]; toAscii(n, c->name, sizeof(n));
  header(n);
  uint8_t items[12];
  int cnt = contactMenuItems(c, items);
  drawList(SCR_CONTACT_MENU, cnt, 24, &UITask::rowContactMenu);
  footer("W/S move  Ent select  Q back");
}

void UITask::keyContactMenu(int k) {
  ContactInfo* c = curContact();
  if (!c) { back(); return; }
  uint8_t items[12];
  int cnt = contactMenuItems(c, items);
  int& sel = _sel[SCR_CONTACT_MENU];
  if (listNav(k, sel, cnt)) return;
  if (isBack(k)) { back(); return; }
  if (k != MK_ENTER && k != ' ') return;
  switch (items[sel]) {
    case CM_MESSAGE:
    case CM_ROOM_CHAT:
      openChat(keyForContact(*c));
      break;
    case CM_LOGIN:
    case CM_ADMIN:
      if (memcmp(_adm.pub, c->id.pub_key, 6) != 0) { memset(&_adm, 0, sizeof(_adm)); memcpy(_adm.pub, c->id.pub_key, 6); }
      _sel[SCR_ADMIN] = 0;
      go(SCR_ADMIN);
      if (!_adm.logged_in) openInput(P_LOGIN_PW, c->type == ADV_TYPE_ROOM ? "Room password (blank=guest)" : "Admin password (blank=guest)", _pw_cache, 20);
      break;
    case CM_INFO: go(SCR_INFO); break;
    case CM_RESET_PATH:
      the_mesh.resetPathTo(*c);
      the_mesh.uiSaveContactsSoon();
      toast("Path reset - next message floods");
      break;
    case CM_SHARE:
      toast(the_mesh.shareContactZeroHop(*c) ? "Contact shared" : "Share failed");
      break;
    case CM_FAV:
      c->flags ^= 0x01;
      the_mesh.uiSaveContactsSoon();
      break;
    case CM_DELETE: {
      char t[80], n[32]; toAscii(n, c->name, sizeof(n));
      snprintf(t, sizeof(t), "Delete contact '%s'?", n);
      openConfirm(P_DEL_CONTACT, t);
      break;
    }
  }
}

void UITask::drawInfo() {
  ContactInfo* c = curContact();
  if (!c) { back(); header("Info"); return; }
  char n[40]; toAscii(n, c->name, sizeof(n));
  header(n);
  font(0);
  int y = HDR_H + (_toast[0] ? 18 : 6);
  char line[64], hx[70];
  const char* type = c->type == ADV_TYPE_CHAT ? "Companion" : c->type == ADV_TYPE_REPEATER ? "Repeater" :
                     c->type == ADV_TYPE_ROOM ? "Room server" : c->type == ADV_TYPE_SENSOR ? "Sensor" : "Unknown";
  snprintf(line, sizeof(line), "Type: %s", type); putText(g(), 4, y, line); y += 13;
  hexStr(hx, c->id.pub_key, 16);
  putText(g(), 4, y, "Public key:"); y += 11;
  char a[40]; memcpy(a, hx, 32); a[32] = 0; putText(g(), 4, y, a); y += 11;
  hexStr(hx, c->id.pub_key + 16, 16); memcpy(a, hx, 32); a[32] = 0; putText(g(), 4, y, a); y += 15;
  if (c->out_path_len == OUT_PATH_UNKNOWN) strcpy(line, "Path: unknown (flood)");
  else {
    int cnt = c->out_path_len & 63, sz = (c->out_path_len >> 6) + 1;
    int p = snprintf(line, sizeof(line), "Path: %d hop%s ", cnt, cnt == 1 ? "" : "s");
    for (int i = 0; i < cnt && p < 58; i++) {
      char b[8]; hexStr(b, &c->out_path[i * sz], 1);
      p += snprintf(line + p, sizeof(line) - p, "%s%s", i ? ">" : "", b);
    }
  }
  putText(g(), 4, y, line); y += 13;
  char age[10]; fmtAge(c->lastmod, age);
  snprintf(line, sizeof(line), "Last heard: %s ago", age); putText(g(), 4, y, line); y += 13;
  if (c->gps_lat || c->gps_lon) {
    double lat = c->gps_lat / 1e6, lon = c->gps_lon / 1e6;
    snprintf(line, sizeof(line), "Location: %.5f, %.5f", lat, lon); putText(g(), 4, y, line); y += 13;
    if (_sensors->node_lat != 0 || _sensors->node_lon != 0) {
      double dlat = (lat - _sensors->node_lat) * DEG_TO_RAD, dlon = (lon - _sensors->node_lon) * DEG_TO_RAD;
      double aa = sin(dlat / 2) * sin(dlat / 2) + cos(_sensors->node_lat * DEG_TO_RAD) * cos(lat * DEG_TO_RAD) * sin(dlon / 2) * sin(dlon / 2);
      double km = 6371.0 * 2 * atan2(sqrt(aa), sqrt(1 - aa));
      double brg = atan2(sin(dlon) * cos(lat * DEG_TO_RAD), cos(_sensors->node_lat * DEG_TO_RAD) * sin(lat * DEG_TO_RAD) -
                         sin(_sensors->node_lat * DEG_TO_RAD) * cos(lat * DEG_TO_RAD) * cos(dlon)) * RAD_TO_DEG;
      if (brg < 0) brg += 360;
      static const char* dirs[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
      snprintf(line, sizeof(line), "Distance: %.2f km (%.1f mi) %s", km, km * 0.621371, dirs[((int)((brg + 22.5) / 45)) & 7]);
      putText(g(), 4, y, line); y += 13;
    }
  } else { putText(g(), 4, y, "Location: not shared"); y += 13; }
  snprintf(line, sizeof(line), "Favourite: %s", c->isFav() ? "yes" : "no"); putText(g(), 4, y, line);
  footer("Q back");
}

void UITask::keyInfo(int k) { if (isBack(k) || k == MK_ENTER) back(); }

// ======================================================================
//  ADMIN (repeaters / rooms)
// ======================================================================

enum { AD_LOGIN = 1, AD_STATUS, AD_ADVERT, AD_CLOCK_SYNC, AD_CLOCK, AD_NEIGH, AD_VER, AD_RADIO, AD_TX, AD_NAME, AD_CUSTOM, AD_REBOOT, AD_LOGOUT };

int UITask::adminItems(uint8_t* out) {
  int n = 0;
  if (!_adm.logged_in) { out[n++] = AD_LOGIN; out[n++] = AD_STATUS; return n; }
  out[n++] = AD_STATUS; out[n++] = AD_ADVERT; out[n++] = AD_CLOCK_SYNC; out[n++] = AD_CLOCK;
  out[n++] = AD_NEIGH; out[n++] = AD_VER; out[n++] = AD_RADIO; out[n++] = AD_TX; out[n++] = AD_NAME;
  out[n++] = AD_CUSTOM; out[n++] = AD_REBOOT; out[n++] = AD_LOGOUT;
  return n;
}

static const char* adLabel(uint8_t id) {
  switch (id) {
    case AD_LOGIN: return "Log in...";
    case AD_STATUS: return "Get status";
    case AD_ADVERT: return "Send advert";
    case AD_CLOCK_SYNC: return "Sync clock to mine";
    case AD_CLOCK: return "Read clock";
    case AD_NEIGH: return "Neighbours";
    case AD_VER: return "Firmware version";
    case AD_RADIO: return "Radio settings";
    case AD_TX: return "TX power";
    case AD_NAME: return "Get name";
    case AD_CUSTOM: return "Custom CLI command...";
    case AD_REBOOT: return "Reboot node";
    case AD_LOGOUT: return "Log out";
  }
  return "?";
}

void UITask::rowAdmin(int i, int y, bool sel) {
  uint8_t items[16]; adminItems(items);
  font(1);
  putText(g(), 8, y + 1, adLabel(items[i]));
}

void UITask::drawAdmin() {
  ContactInfo* c = curContact();
  char n[40]; if (c) toAscii(n, c->name, sizeof(n)); else strcpy(n, "Admin");
  char t[48]; snprintf(t, sizeof(t), "%s%s", n, _adm.logged_in ? (_adm.is_admin ? " [admin]" : " [guest]") : "");
  header(t);
  int y = HDR_H + (_toast[0] ? 16 : 3);
  font(0);
  // status block
  if (_adm.has_status && _adm.status_len >= 40) {
    RemoteStats s; memset(&s, 0, sizeof(s)); memcpy(&s, _adm.status, min((int)sizeof(s), (int)_adm.status_len));
    char l[64];
    uint32_t up = s.total_up_time_secs;
    snprintf(l, sizeof(l), "Batt %.2fV  Up %ud %uh %um", s.batt_milli_volts / 1000.0f, (unsigned)(up / 86400), (unsigned)((up / 3600) % 24), (unsigned)((up / 60) % 60));
    putText(g(), 4, y, l); y += 10;
    snprintf(l, sizeof(l), "Noise %d  RSSI %d  SNR %.1f", s.noise_floor, s.last_rssi, s.last_snr / 4.0f);
    putText(g(), 4, y, l); y += 10;
    snprintf(l, sizeof(l), "RX %u (fl %u/dir %u)", (unsigned)s.n_packets_recv, (unsigned)s.n_recv_flood, (unsigned)s.n_recv_direct);
    putText(g(), 4, y, l); y += 10;
    snprintf(l, sizeof(l), "TX %u (fl %u/dir %u) Q%u", (unsigned)s.n_packets_sent, (unsigned)s.n_sent_flood, (unsigned)s.n_sent_direct, s.curr_tx_queue_len);
    putText(g(), 4, y, l); y += 10;
    snprintf(l, sizeof(l), "Airtime %um  Dups %u/%u  Err %u", (unsigned)(s.total_air_time_secs / 60), s.n_flood_dups, s.n_direct_dups, s.err_events);
    putText(g(), 4, y, l); y += 12;
  } else {
    putText(g(), 4, y, _adm.logged_in ? "No status yet - choose Get status" : "Not logged in"); y += 14;
  }
  g().drawFastHLine(0, y, W, BLACK); y += 2;

  // actions (fixed area)
  uint8_t items[16]; int cnt = adminItems(items);
  int list_h = 6 * 20;
  int& sel = _sel[SCR_ADMIN];
  int& top = _top[SCR_ADMIN];
  sel = constrain(sel, 0, cnt - 1);
  if (sel < top) top = sel;
  if (sel >= top + 6) top = sel - 5;
  for (int i = top; i < cnt && i < top + 6; i++) {
    int ry = y + (i - top) * 20;
    listRow(ry, 20, i == sel);
    rowAdmin(i, ry, i == sel);
    g().setTextColor(BLACK);
  }
  y += list_h + 2;
  g().drawFastHLine(0, y, W, BLACK); y += 3;

  // CLI log
  font(0);
  int bottom = H - FTR_H - 2;
  // draw newest last, wrapping, and fit as many as possible from the bottom
  int lines_avail = (bottom - y) / 10;
  char wrapped[24][40]; int wn = 0;
  for (int i = _adm.log_n - 1; i >= 0 && wn < 24; i--) {
    const char* s = _adm.log[i];
    int L = strlen(s);
    int nl = (L + 38) / 39; if (nl == 0) nl = 1;
    for (int j = nl - 1; j >= 0 && wn < 24; j--) {
      int len = min(39, L - j * 39);
      memcpy(wrapped[wn], s + j * 39, len); wrapped[wn][len] = 0; wn++;
    }
  }
  int show = min(wn, lines_avail);
  for (int i = 0; i < show; i++) putText(g(), 2, y + (show - 1 - i) * 10, wrapped[i]);
  footer("W/S move  Ent run  Q back");
}

void UITask::keyAdmin(int k) {
  ContactInfo* c = curContact();
  if (!c) { back(); return; }
  uint8_t items[16]; int cnt = adminItems(items);
  int& sel = _sel[SCR_ADMIN];
  if (listNav(k, sel, cnt)) return;
  if (isBack(k)) { back(); return; }
  if (k != MK_ENTER && k != ' ') return;
  switch (items[sel]) {
    case AD_LOGIN: openInput(P_LOGIN_PW, "Password (blank=guest)", _pw_cache, 20); break;
    case AD_STATUS: requestStatus(*c); break;
    case AD_ADVERT: sendCli("advert"); break;
    case AD_CLOCK_SYNC: sendCli("clock sync"); break;
    case AD_CLOCK: sendCli("clock"); break;
    case AD_NEIGH: sendCli("neighbors"); break;
    case AD_VER: sendCli("ver"); break;
    case AD_RADIO: sendCli("get radio"); break;
    case AD_TX: sendCli("get tx"); break;
    case AD_NAME: sendCli("get name"); break;
    case AD_CUSTOM: openInput(P_CLI_CUSTOM, "CLI command", "", 100); break;
    case AD_REBOOT: openConfirm(P_CLI_REBOOT, "Reboot this node?"); break;
    case AD_LOGOUT:
      the_mesh.uiStopConnection(c->id.pub_key);
      _adm.logged_in = false; _adm.is_admin = false;
      admLog("Logged out");
      break;
  }
}

// ======================================================================
//  HEARD
// ======================================================================

void UITask::rowHeard(int i, int y, bool sel) {
  AdvertPath& a = _heard[i];
  font(0);
  char right[24], age[8];
  fmtAge(a.recv_timestamp, age);
  int hops = a.path_len & 63;
  snprintf(right, sizeof(right), "%d hop%s %s", hops, hops == 1 ? "" : "s", age);
  int rw = textW(right);
  putText(g(), W - rw - 6, y + 7, right);
  font(1);
  char n[40]; toAscii(n, a.name, sizeof(n));
  while (textW(n) > W - rw - 16 && strlen(n) > 1) n[strlen(n) - 1] = 0;
  putText(g(), 4, y + 3, n);
}

void UITask::drawHeard() {
  _nheard = 0;
  AdvertPath tmp[16];
  int n = the_mesh.getRecentlyHeard(tmp, 16);
  for (int i = 0; i < n; i++) if (tmp[i].name[0] && tmp[i].recv_timestamp) _heard[_nheard++] = tmp[i];
  header("Heard nearby");
  drawList(SCR_HEARD, _nheard, 22, &UITask::rowHeard);
  footer("Ent open contact  Z advert  Q back");
}

void UITask::keyHeard(int k) {
  int& sel = _sel[SCR_HEARD];
  if (listNav(k, sel, _nheard)) return;
  if (isBack(k)) { back(); return; }
  if (k == 'z') { doAdvert(false); return; }
  if ((k == MK_ENTER || k == ' ') && _nheard) {
    ContactInfo* c = the_mesh.lookupContactByPubKey(_heard[sel].pubkey_prefix, 7);
    if (c) { memcpy(_cur_pub, c->id.pub_key, PUB_KEY_SIZE); _sel[SCR_CONTACT_MENU] = 0; go(SCR_CONTACT_MENU); }
    else toast("Not in contacts (auto-add off?)");
  }
}

// ======================================================================
//  GPS
// ======================================================================

void UITask::drawGps() {
  header("GPS & location");
  int y = HDR_H + (_toast[0] ? 18 : 8);
  font(2);
  putText(g(), 6, y, _prefs->gps_enabled ? "GPS: ON" : "GPS: OFF"); y += 24;
  font(0);
  char l[64];
  LocationProvider* lp = ((EnvironmentSensorManager*)_sensors)->getLocationProvider();
  if (_prefs->gps_enabled && lp) {
    snprintf(l, sizeof(l), "Fix: %s   Satellites: %ld", lp->isValid() ? "YES" : "searching...", lp->satellitesCount());
    putText(g(), 6, y, l); y += 13;
    if (lp->isValid()) {
      snprintf(l, sizeof(l), "Lat %.6f  Lon %.6f", lp->getLatitude() / 1e6, lp->getLongitude() / 1e6);
      putText(g(), 6, y, l); y += 13;
      snprintf(l, sizeof(l), "Altitude %.0f m", lp->getAltitude() / 1000.0);
      putText(g(), 6, y, l); y += 13;
    }
  } else {
    putText(g(), 6, y, "GPS is powered off to save battery."); y += 13;
  }
  y += 6;
  snprintf(l, sizeof(l), "My position: %.5f, %.5f", _sensors->node_lat, _sensors->node_lon);
  putText(g(), 6, y, l); y += 13;
  snprintf(l, sizeof(l), "Share position in adverts: %s", _prefs->advert_loc_policy ? "YES" : "no");
  putText(g(), 6, y, l); y += 13;
  snprintf(l, sizeof(l), "Clock: %s", clockValid() ? "set" : "NOT SET (GPS or app sets it)");
  putText(g(), 6, y, l); y += 20;
  font(1);
  putText(g(), 6, y, "G   turn GPS on/off"); y += 20;
  putText(g(), 6, y, "L   share position on/off"); y += 20;
  putText(g(), 6, y, "Z   send advert now"); y += 20;
  putText(g(), 6, y, "X   clear my position");
  footer("Q back");
}

void UITask::keyGps(int k) {
  if (isBack(k)) { back(); return; }
  if (k == 'g') {
    _prefs->gps_enabled = !_prefs->gps_enabled;
    the_mesh.applyGpsPrefs();
    the_mesh.savePrefs();
    gpsPowerSync();
    toast(_prefs->gps_enabled ? "GPS on (fix may take a few minutes)" : "GPS off");
  } else if (k == 'l') {
    _prefs->advert_loc_policy = _prefs->advert_loc_policy ? ADVERT_LOC_NONE : ADVERT_LOC_SHARE;
    the_mesh.savePrefs();
  } else if (k == 'z') {
    doAdvert(false);
  } else if (k == 'x') {
    _sensors->node_lat = 0; _sensors->node_lon = 0;
    the_mesh.savePrefs();
  }
}

// ======================================================================
//  SETTINGS
// ======================================================================

static const char* SETTING_NAMES[NUM_SETTINGS] = {
  "Name", "Radio preset", "Frequency", "Bandwidth", "Spreading factor", "Coding rate", "TX power",
  "Antenna", "Bluetooth", "Bluetooth PIN", "Auto-add contacts", "Share location", "Vibrate",
  "Frontlight", "Keyboard light", "Auto-lock", "Timezone", "Set clock", "E-ink full refresh",
  "Spell check", "Clear message history", "Reboot", "Power off"
};

void UITask::rowSetting(int i, int y, bool sel) {
  char v[40] = "";
  switch (i) {
    case 0: toAscii(v, _prefs->node_name, sizeof(v)); break;
    case 1: {
      strcpy(v, "Custom");
      for (int p = 0; p < NUM_PRESETS; p++)
        if (fabsf(_prefs->freq - PRESETS[p].freq) < 0.001f && fabsf(_prefs->bw - PRESETS[p].bw) < 0.01f &&
            _prefs->sf == PRESETS[p].sf && _prefs->cr == PRESETS[p].cr) strcpy(v, PRESETS[p].name);
      break;
    }
    case 2: sprintf(v, "%.3f MHz", _prefs->freq); break;
    case 3: sprintf(v, "%g kHz", _prefs->bw); break;
    case 4: sprintf(v, "SF%d", _prefs->sf); break;
    case 5: sprintf(v, "4/%d", _prefs->cr); break;
    case 6: sprintf(v, "%d dBm", _prefs->tx_power_dbm); break;
    case 7: strcpy(v, board.isExternalAntenna() ? "External" : "Internal"); break;
    case 8: strcpy(v, isBluetoothEnabled() ? (hasConnection() ? "On (connected)" : "On") : "Off"); break;
    case 9: sprintf(v, "%06u", (unsigned)the_mesh.getBLEPin()); break;
    case 10: strcpy(v, (_prefs->manual_add_contacts & 1) ? "Off" : (_prefs->autoadd_config & 0x01) ? "Replace oldest" : "Until full"); break;
    case 11: strcpy(v, _prefs->advert_loc_policy ? "On" : "Off"); break;
    case 12: strcpy(v, VIBE_NAMES[_cfg.vibrate % 3]); break;
    case 13: strcpy(v, LIGHT_NAMES[_cfg.frontlight & 3]); break;
    case 14: strcpy(v, _cfg.kb_light ? "On" : "Off"); break;
    case 15: if (_cfg.lock_secs == 0) strcpy(v, "Never"); else if (_cfg.lock_secs < 60) sprintf(v, "%us", _cfg.lock_secs); else sprintf(v, "%um", _cfg.lock_secs / 60); break;
    case 16: sprintf(v, "UTC%+d", _prefs->tz_offset); break;
    case 17: if (clockValid()) { uint32_t t = localNow(); time_t tt = t; struct tm x; gmtime_r(&tt, &x); sprintf(v, "%04d-%02d-%02d %02d:%02d", x.tm_year + 1900, x.tm_mon + 1, x.tm_mday, x.tm_hour, x.tm_min); } else strcpy(v, "not set"); break;
    case 18: sprintf(v, "every %d", _cfg.full_every); break;
    case 19: strcpy(v, _cfg.spell_off ? "Off" : "On"); break;
  }
  font(1);
  putText(g(), 4, y + 2, SETTING_NAMES[i]);
  if (v[0]) {
    font(0);
    while (textW(v) > 110 && strlen(v) > 1) v[strlen(v) - 1] = 0;
    putText(g(), W - textW(v) - 8, y + 7, v);
  }
}

void UITask::drawSettings() {
  header("Settings");
  drawList(SCR_SETTINGS, NUM_SETTINGS, 22, &UITask::rowSetting);
  footer("W/S move  Ent change  Q back");
}

void UITask::keySettings(int k) {
  int& sel = _sel[SCR_SETTINGS];
  if (listNav(k, sel, NUM_SETTINGS)) return;
  if (isBack(k)) { back(); return; }
  if (k != MK_ENTER && k != ' ') return;
  char buf[24];
  switch (sel) {
    case 0: openInput(P_SET_NAME, "Your name", _prefs->node_name, 31); break;
    case 1: {
      for (int i = 0; i < NUM_PRESETS; i++) StrHelper::strncpy(_m_optbuf[i], PRESETS[i].name, 28);
      openPick(P_SET_PRESET, "Radio preset", NUM_PRESETS, 0);
      break;
    }
    case 2: sprintf(buf, "%.3f", _prefs->freq); openInput(P_SET_FREQ, "Frequency (MHz)", buf, 10, true); break;
    case 3: {
      int cur = 0;
      for (int i = 0; i < 10; i++) { sprintf(_m_optbuf[i], "%g kHz", BWS[i]); if (fabsf(BWS[i] - _prefs->bw) < 0.05f) cur = i; }
      openPick(P_SET_BW, "Bandwidth", 10, cur);
      break;
    }
    case 4: for (int i = 0; i < 8; i++) sprintf(_m_optbuf[i], "SF%d", i + 5); openPick(P_SET_SF, "Spreading factor", 8, _prefs->sf - 5); break;
    case 5: for (int i = 0; i < 4; i++) sprintf(_m_optbuf[i], "4/%d", i + 5); openPick(P_SET_CR, "Coding rate", 4, _prefs->cr - 5); break;
    case 6: for (int i = 0; i < 22; i++) sprintf(_m_optbuf[i], "%d dBm", 22 - i); openPick(P_SET_TX, "TX power", 22, 22 - _prefs->tx_power_dbm); break;
    case 7: board.setExternalAntenna(!board.isExternalAntenna()); toast(board.isExternalAntenna() ? "External antenna (u.FL)" : "Internal antenna"); break;
    case 8:
      _cfg.ble_on = !isBluetoothEnabled();
      if (_cfg.ble_on) enableBluetooth(); else disableBluetooth();
      saveCfg();
      toast(_cfg.ble_on ? "Bluetooth on" : "Bluetooth off (saves battery)");
      break;
    case 9: sprintf(buf, "%06u", (unsigned)the_mesh.getBLEPin()); openInput(P_SET_PIN, "6-digit BLE PIN", buf, 6, true); break;
    case 10: {
      strcpy(_m_optbuf[0], "Off"); strcpy(_m_optbuf[1], "On, until full"); strcpy(_m_optbuf[2], "On, replace oldest");
      int cur = (_prefs->manual_add_contacts & 1) ? 0 : (_prefs->autoadd_config & 0x01) ? 2 : 1;
      openPick(P_SET_AUTOADD, "Auto-add (favs never replaced)", 3, cur);
      break;
    }
    case 11: _prefs->advert_loc_policy = _prefs->advert_loc_policy ? ADVERT_LOC_NONE : ADVERT_LOC_SHARE; the_mesh.savePrefs(); break;
    case 12: for (int i = 0; i < 3; i++) strcpy(_m_optbuf[i], VIBE_NAMES[i]); openPick(P_SET_VIBE, "Vibrate on", 3, _cfg.vibrate); break;
    case 13: for (int i = 0; i < 4; i++) strcpy(_m_optbuf[i], LIGHT_NAMES[i]); openPick(P_SET_LIGHT, "Frontlight", 4, _cfg.frontlight); break;
    case 14: _cfg.kb_light = !_cfg.kb_light; saveCfg(); applyLights(); break;
    case 15: {
      int cur = 0;
      for (int i = 0; i < 6; i++) {
        if (LOCK_OPTS[i] == 0) strcpy(_m_optbuf[i], "Never");
        else if (LOCK_OPTS[i] < 60) sprintf(_m_optbuf[i], "%u seconds", LOCK_OPTS[i]);
        else sprintf(_m_optbuf[i], "%u minutes", LOCK_OPTS[i] / 60);
        if (LOCK_OPTS[i] == _cfg.lock_secs) cur = i;
      }
      openPick(P_SET_LOCK, "Auto-lock after", 6, cur);
      break;
    }
    case 16: sprintf(buf, "%d", _prefs->tz_offset); openInput(P_SET_TZ, "UTC offset hours (e.g. -5)", buf, 3); break;
    case 17: openInput(P_SET_CLOCK, "Local time YYYY-MM-DD HH:MM", "", 16); break;
    case 18: {
      static const int opts[] = { 5, 10, 25, 50, 100 };
      int cur = 2;
      for (int i = 0; i < 5; i++) { sprintf(_m_optbuf[i], "every %d updates", opts[i]); if (opts[i] == _cfg.full_every) cur = i; }
      openPick(P_SET_REFRESH, "Full refresh (anti-ghost)", 5, cur);
      break;
    }
    case 19: _cfg.spell_off = !_cfg.spell_off; saveCfg(); toast(_cfg.spell_off ? "Spell check off" : "Spell check on (Alt+K = add word)"); break;
    case 20: openConfirm(P_CLEAR_MSGS, "Erase ALL message history on this device?"); break;
    case 21: openConfirm(P_REBOOT, "Reboot now?"); break;
    case 22: openConfirm(P_POWER_OFF, "Power off? (BOOT button turns it back on)"); break;
  }
}

// ======================================================================
//  DEVICE INFO
// ======================================================================

void UITask::drawDevice() {
  header("Device info");
  int y = HDR_H + (_toast[0] ? 18 : 6);
  font(0);
  char l[64], hx[20];
  snprintf(l, sizeof(l), "Firmware: maxmesh %s", MAXMESH_VERSION); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Based on MeshCore %s", FIRMWARE_VERSION); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Build: %s", FIRMWARE_BUILD_DATE); putText(g(), 4, y, l); y += 12;
  hexStr(hx, the_mesh.self_id.pub_key, 8);
  snprintf(l, sizeof(l), "My ID: %s...", hx); putText(g(), 4, y, l); y += 16;
  readBattery();
  snprintf(l, sizeof(l), "Battery: %d%%  %u mV", _batt_pct, _batt_mv); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Current: %d mA %s", _batt_ma, _batt_ma > 0 ? "(charging)" : ""); putText(g(), 4, y, l); y += 16;
  snprintf(l, sizeof(l), "Radio: %.3f MHz BW%g SF%d CR%d", _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "TX %d dBm  Noise floor %d dBm", _prefs->tx_power_dbm, radio_driver.getNoiseFloor()); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Last RSSI %.0f  SNR %.1f", radio_driver.getLastRSSI(), radio_driver.getLastSNR()); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Packets RX %u  TX %u  Err %u", (unsigned)radio_driver.getPacketsRecv(), (unsigned)radio_driver.getPacketsSent(), (unsigned)radio_driver.getPacketsRecvErrors()); putText(g(), 4, y, l); y += 16;
  buildChannels();
  snprintf(l, sizeof(l), "Contacts %d/%d  Channels %d", the_mesh.getNumContacts(), MAX_CONTACTS, s_nchan); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Messages stored %d/%d", _store.count(), MSG_STORE_SIZE); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Storage %u/%u KB", (unsigned)(SPIFFS.usedBytes() / 1024), (unsigned)(SPIFFS.totalBytes() / 1024)); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "Free heap %u KB  PSRAM %u KB", (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getFreePsram() / 1024)); putText(g(), 4, y, l); y += 12;
  uint32_t up = millis() / 1000;
  snprintf(l, sizeof(l), "Uptime %ud %uh %um", (unsigned)(up / 86400), (unsigned)((up / 3600) % 24), (unsigned)((up / 60) % 60)); putText(g(), 4, y, l); y += 12;
  snprintf(l, sizeof(l), "BLE: %s  PIN %06u", isBluetoothEnabled() ? (hasConnection() ? "connected" : "on") : "off", (unsigned)the_mesh.getBLEPin()); putText(g(), 4, y, l);
  footer("Q back");
}

void UITask::keyDevice(int k) { if (isBack(k) || k == MK_ENTER) back(); }

// ======================================================================
//  LOCK SCREEN
// ======================================================================

void UITask::drawLock() {
  g().setTextColor(BLACK);
  char t[16];
  if (clockValid()) {
    font(5);
    uint32_t n = localNow(); sprintf(t, "%u:%02u", (unsigned)((n / 3600) % 24), (unsigned)((n / 60) % 60));
    putText(g(), (W - textW(t)) / 2, 50, t);
  } else {
    font(2);
    putText(g(), (W - textW("Clock not set")) / 2, 60, "Clock not set");
  }
  font(1);
  if (clockValid()) {
    time_t n = localNow(); struct tm x; gmtime_r(&n, &x);
    static const char* days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char* mons[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char d[40]; sprintf(d, "%s, %s %d", days[x.tm_wday], mons[x.tm_mon], x.tm_mday);
    putText(g(), (W - textW(d)) / 2, 100, d);
  }
  char nm[40]; toAscii(nm, _prefs->node_name, sizeof(nm));
  font(2);
  putText(g(), (W - textW(nm)) / 2, 135, nm);

  // unread summary
  buildConvs();
  int total = _store.totalUnread();
  int y = 175;
  font(1);
  if (total == 0) {
    putText(g(), (W - textW("No new messages")) / 2, y, "No new messages");
  } else {
    char s[40]; sprintf(s, "%d new message%s", total, total == 1 ? "" : "s");
    font(2); putText(g(), (W - textW(s)) / 2, y, s); y += 26;
    font(0);
    for (int i = 0, shown = 0; i < _nconv && shown < 5; i++) {
      if (!_convs[i].unread) continue;
      char n[32], l[48]; convName(_convs[i].key, n, sizeof(n));
      snprintf(l, sizeof(l), "%s%s  (%d)", (_convs[i].key.kind == CONV_CHANNEL && n[0] != '#') ? "#" : "", n, _convs[i].unread);
      putText(g(), 20, y, l); y += 12; shown++;
    }
  }
  // bottom status
  font(0);
  char b[64];
  snprintf(b, sizeof(b), "%s  %s%s", _usb ? "Charging" : "", _batt_pct >= 0 ? "" : "", "");
  if (_batt_pct >= 0) snprintf(b, sizeof(b), "Battery %d%%%s   BT %s", _batt_pct, _usb ? " (USB)" : "", isBluetoothEnabled() ? (hasConnection() ? "connected" : "on") : "off");
  putText(g(), (W - textW(b)) / 2, H - 34, b);
  putText(g(), (W - textW("Press the side button to unlock")) / 2, H - 18, "Press the side button to unlock");
}

// ======================================================================
//  MODALS
// ======================================================================

void UITask::drawModal() {
  int x = 8, w = W - 16;
  if (_modal == MODAL_INPUT) {
    font(1);
    int lines = min(5, wrapText(_m_buf[0] ? _m_buf : " ", 0, 0, w - 16, 18, false) + (textW(_m_buf) % (w - 16) > w - 30 ? 1 : 0));
    int h = 40 + lines * 18 + 14;
    int y = 70;
    g().fillRect(x, y, w, h, WHITE);
    g().drawRect(x, y, w, h, BLACK); g().drawRect(x + 1, y + 1, w - 2, h - 2, BLACK);
    g().fillRect(x, y, w, 20, BLACK);
    g().setTextColor(WHITE); font(0); putText(g(), x + 6, y + 6, _m_title); g().setTextColor(BLACK);
    font(1);
    char tmp[sizeof(_m_buf) + 2]; snprintf(tmp, sizeof(tmp), "%s|", _m_buf);
    const char* p = tmp;
    while (*p && wrapText(p, 0, 0, w - 16, 18, false) > 5) p++;
    wrapText(p, x + 8, y + 26, w - 16, 18, true, 5);
    font(0);
    putText(g(), x + 6, y + h - 12, "Ent=OK   Alt+Q=cancel");
  } else if (_modal == MODAL_CONFIRM) {
    font(1);
    // split on '\n'
    char buf[200]; StrHelper::strncpy(buf, _m_text, sizeof(buf));
    int total = 0; char* parts[6]; int np = 0;
    for (char* s = strtok(buf, "\n"); s && np < 6; s = strtok(nullptr, "\n")) { parts[np++] = s; total += wrapText(s, 0, 0, w - 16, 18, false); }
    int h = 20 + total * 18 + 30;
    int y = max(30, (H - h) / 2);
    g().fillRect(x, y, w, h, WHITE);
    g().drawRect(x, y, w, h, BLACK); g().drawRect(x + 1, y + 1, w - 2, h - 2, BLACK);
    int ty = y + 12;
    for (int i = 0; i < np; i++) ty += wrapText(parts[i], x + 8, ty, w - 16, 18, true) * 18;
    font(0);
    putText(g(), x + 8, y + h - 14, _purpose == P_NONE ? "Press any key to close" : "Y = yes        N / Q = no");
  } else if (_modal == MODAL_PICK) {
    int rows = min(_m_nopts, 9);
    int h = 22 + rows * 22 + 4;
    int y = max(24, (H - h) / 2);
    g().fillRect(x, y, w, h, WHITE);
    g().drawRect(x, y, w, h, BLACK); g().drawRect(x + 1, y + 1, w - 2, h - 2, BLACK);
    g().fillRect(x, y, w, 20, BLACK);
    g().setTextColor(WHITE); font(0); putText(g(), x + 6, y + 6, _m_title); g().setTextColor(BLACK);
    font(1);
    for (int i = _m_top; i < _m_nopts && i < _m_top + rows; i++) {
      int ry = y + 22 + (i - _m_top) * 22;
      if (i == _m_sel) { g().fillRect(x + 3, ry, w - 6, 22, BLACK); g().setTextColor(WHITE); }
      putText(g(), x + 10, ry + 3, _m_opts[i]);
      g().setTextColor(BLACK);
    }
  }
}

void UITask::modalDone(bool ok) {
  Modal was = _modal;
  _modal = MODAL_NONE;
  _dirty = true;
  if (!ok) {
    if (_purpose == P_LOGIN_PW && was == MODAL_INPUT) { /* stay on admin screen, not logged in */ }
    return;
  }
  ContactInfo* c = curContact();
  switch (_purpose) {
    case P_SET_NAME:
      if (_m_buf[0]) {
        StrHelper::strncpy(_prefs->node_name, _m_buf, sizeof(_prefs->node_name));
        the_mesh.savePrefs();
        toast("Name saved - send an advert");
      }
      break;
    case P_SET_PRESET: applyPreset(_m_sel); break;
    case P_SET_FREQ: {
      float f = atof(_m_buf);
      if (f >= 150 && f <= 960) { _prefs->freq = f; the_mesh.uiApplyRadioParams(); toast("Frequency %.3f", f); }
      else toast("Invalid frequency");
      break;
    }
    case P_SET_BW: _prefs->bw = BWS[_m_sel]; the_mesh.uiApplyRadioParams(); break;
    case P_SET_SF: _prefs->sf = _m_sel + 5; the_mesh.uiApplyRadioParams(); break;
    case P_SET_CR: _prefs->cr = _m_sel + 5; the_mesh.uiApplyRadioParams(); break;
    case P_SET_TX: _prefs->tx_power_dbm = 22 - _m_sel; the_mesh.uiApplyRadioParams(); break;
    case P_SET_PIN: {
      uint32_t pin = strtoul(_m_buf, nullptr, 10);
      if (strlen(_m_buf) == 6 && pin >= 100000) {
        _prefs->ble_pin = pin; the_mesh.savePrefs();
        openConfirm(P_REBOOT, "PIN saved. Reboot now to apply?");
      } else toast("PIN must be 6 digits");
      break;
    }
    case P_SET_VIBE: _cfg.vibrate = _m_sel; saveCfg(); if (_m_sel) board.buzz(14); break;
    case P_SET_LIGHT: _cfg.frontlight = _m_sel; saveCfg(); applyLights(); break;
    case P_SET_LOCK: _cfg.lock_secs = LOCK_OPTS[_m_sel]; saveCfg(); break;
    case P_SET_AUTOADD:
      if (_m_sel == 0) { _prefs->manual_add_contacts |= 1; }
      else {
        _prefs->manual_add_contacts &= ~1;   // add every contact type
        if (_m_sel == 2) _prefs->autoadd_config |= 0x01;    // AUTO_ADD_OVERWRITE_OLDEST (skips favourites)
        else _prefs->autoadd_config &= ~0x01;
      }
      the_mesh.savePrefs();
      break;
    case P_SET_REFRESH: { static const int opts[] = { 5, 10, 25, 50, 100 }; _cfg.full_every = opts[_m_sel]; saveCfg(); _disp->setFullRefreshEvery(_cfg.full_every); break; }
    case P_SET_TZ: { int tz = atoi(_m_buf); if (tz >= -12 && tz <= 14) { _prefs->tz_offset = tz; the_mesh.savePrefs(); } break; }
    case P_SET_CLOCK: {
      int Y, M, D, h, m;
      if (sscanf(_m_buf, "%d-%d-%d %d:%d", &Y, &M, &D, &h, &m) == 5 && Y >= 2024 && M >= 1 && M <= 12 && D >= 1 && D <= 31) {
        int64_t days = daysFromCivil(Y, M, D);
        uint32_t epoch = (uint32_t)(days * 86400 + h * 3600 + m * 60 - (int32_t)_prefs->tz_offset * 3600);
        rtc_clock.setCurrentTime(epoch);
        toast("Clock set");
      } else toast("Format: 2026-11-24 18:30");
      break;
    }
    case P_NEW_CHANNEL: {
      char name[32]; StrHelper::strncpy(name, _m_buf, sizeof(name));
      if (!name[0] || strcmp(name, "#") == 0) break;
      int slot = -1;
      for (int i = 0; i < MAX_GROUP_CHANNELS; i++) { ChannelDetails ch; if (the_mesh.getChannel(i, ch) && !ch.name[0]) { slot = i; break; } }
      if (slot < 0) { toast("Channel list is full"); break; }
      if (strcasecmp(name, "public") == 0) {
        // the standard MeshCore public channel
        ChannelDetails ch; memset(&ch, 0, sizeof(ch));
        strcpy(ch.name, "Public");
        decode_base64((const unsigned char*)"izOH6cXN6mrJ5e26oRXNcg==", 24, ch.channel.secret);
        the_mesh.setChannel(slot, ch);
        the_mesh.uiSaveChannels();
        toast("Public channel added");
        break;
      }
      if (name[0] == '#') {
        for (char* p = name; *p; p++) *p = tolower(*p);
        ChannelDetails ch; memset(&ch, 0, sizeof(ch));
        StrHelper::strncpy(ch.name, name, sizeof(ch.name));
        uint8_t hash[32];
        mesh::Utils::sha256(hash, 32, (const uint8_t*)name, strlen(name));
        memcpy(ch.channel.secret, hash, 16);
        the_mesh.setChannel(slot, ch);
        the_mesh.uiSaveChannels();
        toast("Joined %s", name);
      } else {
        // private channel: ask for its key (blank = make a new random one)
        StrHelper::strncpy(_m_title, name, sizeof(_m_title));   // remember name
        StrHelper::strncpy(_find, name, sizeof(_find));
        openInput(P_NEW_CHANNEL_KEY, "Key base64 (blank=new)", "", 48);
        return;
      }
      break;
    }
    case P_NEW_CHANNEL_KEY: {
      int slot = -1;
      for (int i = 0; i < MAX_GROUP_CHANNELS; i++) { ChannelDetails ch; if (the_mesh.getChannel(i, ch) && !ch.name[0]) { slot = i; break; } }
      if (slot < 0) { toast("Channel list is full"); break; }
      ChannelDetails ch; memset(&ch, 0, sizeof(ch));
      StrHelper::strncpy(ch.name, _find, sizeof(ch.name));
      _find[0] = 0;
      if (_m_buf[0]) {
        uint8_t key[40];
        int n = decode_base64((unsigned char*)_m_buf, strlen(_m_buf), key);
        if (n != 16 && n != 32) { toast("Key must be 16 or 32 bytes base64"); break; }
        memcpy(ch.channel.secret, key, n);
      } else {
        esp_fill_random(ch.channel.secret, 16);
      }
      the_mesh.setChannel(slot, ch);
      the_mesh.uiSaveChannels();
      toast("Channel '%s' added (I = show key)", ch.name);
      break;
    }
    case P_DEL_CHANNEL: {
      buildChannels();
      int sel = _sel[SCR_CHANNELS];
      if (sel < s_nchan) {
        int idx = s_chan_idx[sel];
        _store.eraseConv(keyForChannel(idx));
        ChannelDetails ch; memset(&ch, 0, sizeof(ch));
        the_mesh.setChannel(idx, ch);
        the_mesh.uiSaveChannels();
        toast("Channel deleted");
      }
      break;
    }
    case P_DEL_CONV: _store.eraseConv(_chat); break;
    case P_DEL_CONTACT:
      if (c) {
        uint8_t pk[PUB_KEY_SIZE]; memcpy(pk, c->id.pub_key, PUB_KEY_SIZE);
        if (the_mesh.removeContact(*c)) {
          the_mesh.uiDeleteContactBlob(pk);
          the_mesh.uiSaveContactsSoon();
          buildContactList();
          toast("Contact deleted");
          back();
        }
      }
      break;
    case P_LOGIN_PW: if (c) sendLogin(*c, _m_buf); break;
    case P_CLI_CUSTOM: if (_m_buf[0]) sendCli(_m_buf); break;
    case P_CLI_REBOOT: sendCli("reboot"); break;
    case P_FIND: StrHelper::strncpy(_find, _m_buf, sizeof(_find)); _sel[SCR_CONTACTS] = 0; buildContactList(); break;
    case P_CLEAR_MSGS: {
      while (_store.count()) { ConvKey k = _store.at(0)->conv; _store.eraseConv(k); }
      _store.saveIfNeeded(true);
      toast("Message history cleared");
      break;
    }
    case P_REBOOT: _store.saveIfNeeded(true); the_mesh.savePrefs(); delay(100); ESP.restart(); break;
    case P_POWER_OFF: _btn_long_done = false; _store.saveIfNeeded(true); the_mesh.savePrefs();
      _disp->startFrame(); font(3); g().setCursor(20, 150); g().print("Powered off");
      _disp->forceFullRefresh(); _disp->commit(); board.powerOff(); break;
    default: break;
  }
}
