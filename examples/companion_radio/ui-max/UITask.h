#pragma once

// Full on-device UI for the LilyGo T-Deck MAX (e-paper + QWERTY keyboard).

#include "../AbstractUITask.h"
#include "MsgStore.h"
#include "SpellCheck.h"
#include <target.h>

#define UI_MAX_LIST 96

enum Screen : uint8_t {
  SCR_HOME, SCR_CONVS, SCR_CHAT, SCR_CHANNELS, SCR_CONTACTS, SCR_CONTACT_MENU,
  SCR_INFO, SCR_ADMIN, SCR_HEARD, SCR_GPS, SCR_SETTINGS, SCR_DEVICE, SCR_LOCK
};

enum Modal : uint8_t { MODAL_NONE, MODAL_INPUT, MODAL_CONFIRM, MODAL_PICK };

// what a modal's result is for
enum Purpose : uint8_t {
  P_NONE, P_SET_NAME, P_SET_FREQ, P_SET_BW, P_SET_SF, P_SET_CR, P_SET_TX, P_SET_PRESET,
  P_SET_PIN, P_SET_TZ, P_SET_LOCK, P_SET_AUTOADD, P_SET_LIGHT, P_SET_VIBE, P_SET_REFRESH, P_SET_CLOCK,
  P_NEW_CHANNEL, P_NEW_CHANNEL_KEY, P_DEL_CHANNEL, P_DEL_CONV, P_DEL_CONTACT,
  P_LOGIN_PW, P_CLI_CUSTOM, P_CLI_REBOOT, P_FIND, P_POWER_OFF, P_REBOOT, P_CLEAR_MSGS,
  P_CONTACT_ACTION, P_NEW_MSG_CONTACT
};

struct UISettings {
  uint32_t magic;
  uint8_t ble_on;
  uint8_t vibrate;        // 0 off, 1 DMs only, 2 everything
  uint8_t frontlight;     // 0..3
  uint8_t kb_light;       // 0/1
  uint16_t lock_secs;     // 0 = never
  uint8_t full_every;     // full e-ink refresh after N partials
  uint8_t welcomed;
  uint8_t spell_off;      // 1 = no spell-check underlines
  uint8_t gps_no_tsync;   // 1 = don't use GPS to fix the clock after a power loss (0 = auto sync on)
  uint8_t reserved[14];
};

struct AdminState {
  uint8_t  pub[6];
  bool     logged_in;
  bool     is_admin;
  bool     has_status;
  uint8_t  status[64];
  uint8_t  status_len;
  uint32_t status_time;
  char     log[6][120];   // last CLI replies
  int      log_n;
};

class UITask : public AbstractUITask {
  TDeckMaxDisplay* _disp = nullptr;
  SensorManager* _sensors = nullptr;
  NodePrefs* _prefs = nullptr;
  MsgStore _store;
  UISettings _cfg;

  // navigation
  Screen _scr = SCR_HOME;
  Screen _stack[10];
  int _sp = 0;
  int _sel[13] = { 0 };       // selection per screen
  int _top[13] = { 0 };       // first visible row per screen
  bool _dirty = true;
  bool _need_full = false;

  // modal
  Modal _modal = MODAL_NONE;
  Purpose _purpose = P_NONE;
  char _m_title[40];
  char _m_text[200];
  char _m_buf[170];
  int  _m_max = 0;
  bool _m_numeric = false;
  const char* _m_opts[24];
  char _m_optbuf[24][28];
  int  _m_nopts = 0, _m_sel = 0, _m_top = 0;

  // chat
  ConvKey _chat;
  char _input[MSG_TEXT_MAX];
  int _scroll = 0;            // messages hidden below (scroll back)
  int _chat_room_ch = -1;     // channel index for channel chats
  StoredMsg* _new_marker = nullptr;   // first message that was unread when the chat was opened
  bool _jump_top = false;
  uint32_t _new_marker_ts = 0;        // (validates the slot wasn't reused)

  // contacts
  int _ct_filter = 0;         // 0 all, 1 people, 2 repeaters, 3 rooms, 4 favs
  char _find[24];
  int _list_idx[UI_MAX_LIST]; // contact-slot indexes for the current list
  int _list_n = 0;
  uint8_t _cur_pub[PUB_KEY_SIZE];   // contact being acted on
  bool _pick_for_msg = false;

  // admin / requests
  AdminState _adm;
  uint8_t _pend_kind = 0;     // 1 login, 2 status
  uint8_t _pend_pub[4];
  uint32_t _pend_deadline = 0;
  char _pw_cache[24];

  // notifications / timing
  char _toast[60];
  uint32_t _toast_until = 0;
  uint32_t _last_input = 0;
  uint32_t _last_minute = 0;
  uint32_t _batt_next = 0;
  int  _batt_pct = -1;
  uint16_t _batt_mv = 0;
  int  _batt_ma = 0;
  bool _usb = false;
  uint32_t _btn_down = 0;
  bool _btn_prev = false;
  bool _btn_long_done = false;
  uint32_t _btn_last_toggle = 0;
  bool _gps_pow = false;
  uint32_t _boot_ms = 0;
  bool _in_busy = false;
  bool _has_slept = false;    // light-sleep breaks the USB link until a restart
  uint32_t _time_saved_at = 0;
  // GPS auto time sync: after a start that lost the clock (power loss / flat battery / reset), the clock is
  // "suspect" until GPS (or the app, or a manual set) fixes it. GPS is powered just for that, then off again.
  bool _clock_suspect = false;
  bool _tsync_active = false;      // GPS powered by us (user GPS setting is off)
  bool _tsync_failed = false;      // last attempt got no fix (waiting to retry)
  uint32_t _tsync_next = 0;        // millis() of the next attempt
  uint32_t _tsync_started = 0, _tsync_valid_since = 0;
  uint32_t _tsync_ref_rtc = 0, _tsync_ref_ms = 0;   // to notice the clock being set by someone else
  double _tsync_lat = 0, _tsync_lon = 0;
  void timeSyncLoop();
  void timeSyncStart();
  void timeSyncStop(bool ok);
  void timeSyncRef();
  void usbPluggedAfterSleep();
  void checkLowBattery(bool at_boot);
  void lowBatteryShutdown();
  uint16_t _fake_batt_mv = 0;   // test console: simulate a battery voltage
  uint8_t _low_count = 0;
  bool _low_warned = false;

  // serial test console ("!status", "!keys ...", "!shot")
  char _ser[200];
  int _ser_n = 0;
  int _inj[96];
  int _inj_h = 0, _inj_t = 0;
  void serialPoll();
  void serialCommand(char* line);
  void printStatus();
  int  nextKey();

  // ---- helpers ----
  void go(Screen s);
  void back();
  void toast(const char* fmt, ...);
  void loadCfg();
  void saveCfg();
  void applyLights();
  void openChat(const ConvKey& k);
  bool convName(const ConvKey& k, char* dest, int n);
  int  channelForKey(const ConvKey& k);
  ConvKey keyForChannel(int idx);
  ConvKey keyForContact(const ContactInfo& c);
  ContactInfo* curContact();
  void buildContactList();
  bool sendChat();
  void retryLoop();
  void requestStatus(ContactInfo& c);
  void sendLogin(ContactInfo& c, const char* pw);
  void sendCli(const char* cmd);
  void admLog(const char* line);
  void doAdvert(bool flood);
  void applyPreset(int i);
  void setLockSecs(int idx);
  bool clockValid();
  uint32_t localNow();
  void fmtTime(uint32_t t, char* out);
  void fmtAge(uint32_t t, char* out);
  void readBattery();
  void handleButton();
  void gpsPowerSync();
  void notifyIncoming(bool is_dm, const char* from, const char* text);

  // modals
  void openInput(Purpose p, const char* title, const char* init, int max_len, bool numeric = false);
  void openConfirm(Purpose p, const char* text);
  void openPick(Purpose p, const char* title, int n, int sel);
  void modalKey(int k);
  void modalDone(bool ok);

  // drawing
  Adafruit_GFX& g() { return _disp->gfx(); }
  void font(int f);
  int  textW(const char* s);
  void header(const char* title);
  void footer(const char* hint);
  int  wrapText(const char* text, int x, int y, int w, int line_h, bool draw, int max_lines = 99, bool spellcheck = false);
  void listRow(int y, int h, bool sel);
  void drawList(int scr, int n, int row_h, void (UITask::*row)(int idx, int y, bool sel));

  void render();
  void drawHome();     void keyHome(int k);
  void drawConvs();    void keyConvs(int k);
  void drawChat();     void keyChat(int k);
  void drawChannels(); void keyChannels(int k);
  void drawContacts(); void keyContacts(int k);
  void drawContactMenu(); void keyContactMenu(int k);
  void drawInfo();     void keyInfo(int k);
  void drawAdmin();    void keyAdmin(int k);
  void drawHeard();    void keyHeard(int k);
  void drawGps();      void keyGps(int k);
  void drawSettings(); void keySettings(int k);
  void drawDevice();   void keyDevice(int k);
  void drawLock();
  void drawModal();

  void rowHome(int i, int y, bool sel);
  void rowConv(int i, int y, bool sel);
  void rowChannel(int i, int y, bool sel);
  void rowContact(int i, int y, bool sel);
  void rowSetting(int i, int y, bool sel);
  void rowContactMenu(int i, int y, bool sel);
  void rowAdmin(int i, int y, bool sel);
  void rowHeard(int i, int y, bool sel);

  // conversation index for SCR_CONVS
  struct ConvEntry { ConvKey key; uint32_t last; int unread; int last_idx; };
  ConvEntry _convs[64];
  int _nconv = 0;
  void buildConvs();

  AdvertPath _heard[16];
  int _nheard = 0;

  int contactMenuItems(const ContactInfo* c, uint8_t* out);
  int adminItems(uint8_t* out);

public:
  UITask(mesh::MainBoard* board, MultiSerialInterface* iface) : AbstractUITask(board, iface) { }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;
  void busyPoll();

  // MyMesh::Listener
  void onMessageRecv(mesh::Packet* pkt, const ContactInfo& from, uint8_t txt_type, uint32_t sender_timestamp, const char* text) override;
  void onSignedMessageRecv(mesh::Packet* pkt, const ContactInfo& from, uint32_t sender_timestamp, const uint8_t* author_prefix, const char* text) override;
  void onChannelMessageRecv(mesh::Packet* pkt, ChannelDetails& channel, const char* text) override;
  void onQueueSizeChanged(int msgcount) override { }
  void onDiscoveredContact(ContactInfo& contact, bool is_new, uint8_t path_len, const uint8_t* path) override;
  void onACKRecv(uint32_t ack_crc) override;
  bool onUnhandledResponse(const ContactInfo& from, uint32_t tag, const uint8_t* data, uint8_t len) override;
};
