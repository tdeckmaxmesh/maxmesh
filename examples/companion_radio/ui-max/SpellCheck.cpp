#include "SpellCheck.h"
#include <esp_heap_caps.h>

SpellCheck spell;

// embedded by platformio.ini: board_build.embed_txtfiles (a NUL is appended)
extern const char dict_start[] asm("_binary_examples_companion_radio_ui_max_spell_words_txt_start");
extern const char dict_end[]   asm("_binary_examples_companion_radio_ui_max_spell_words_txt_end");

#define USER_FILE "/mx_words.txt"

static int cmpLine(const char* line, const char* w, int wlen) {
  // compare dictionary line (terminated by '\n') against w[0..wlen)
  // ('\r' is treated as end-of-line too, in case the file was checked out with CRLF)
  for (int i = 0; i < wlen; i++) {
    unsigned char a = (unsigned char)line[i], b = (unsigned char)w[i];
    if (a == '\n' || a == '\r' || a == 0) return -1;     // line is a prefix of w -> line < w
    if (a != b) return a < b ? -1 : 1;
  }
  char e = line[wlen];
  return (e == '\n' || e == '\r' || e == 0) ? 0 : 1;      // w is a prefix of line -> line > w
}

void SpellCheck::begin(fs::FS& fs) {
  _fs = &fs;
  if (!_user) _user = (char*)heap_caps_calloc(USER_MAX, WORD_MAX, MALLOC_CAP_SPIRAM);
  _nuser = 0;
  File f = fs.open(USER_FILE, "r");
  if (!f || !_user) return;
  while (f.available() && _nuser < USER_MAX) {
    String s = f.readStringUntil('\n');
    s.trim(); s.toLowerCase();
    if (s.length() > 0 && s.length() < WORD_MAX) strcpy(_user + _nuser++ * WORD_MAX, s.c_str());
  }
  f.close();
}

bool SpellCheck::known(const char* word, int len) {
  char w[WORD_MAX + 8];
  // strip leading/trailing apostrophes, lowercase
  while (len > 0 && *word == '\'') { word++; len--; }
  while (len > 0 && word[len - 1] == '\'') len--;
  if (len <= 0) return true;
  if (len >= (int)sizeof(w)) return true;         // very long tokens: don't nag
  for (int i = 0; i < len; i++) w[i] = tolower((unsigned char)word[i]);
  w[len] = 0;

  for (int i = 0; i < _nuser; i++) if (strcmp(_user + i * WORD_MAX, w) == 0) return true;

  const char* base = dict_start;
  int size = (int)(dict_end - dict_start) - 1;     // drop the appended NUL
  int lo = 0, hi = size;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int s = mid;
    while (s > 0 && base[s - 1] != '\n') s--;      // back up to start of this line
    if (s < lo) s = lo;
    int c = cmpLine(base + s, w, len);
    if (c == 0) return true;
    if (c < 0) {                                    // line < word: search after this line
      int e = s;
      while (e < size && base[e] != '\n') e++;
      lo = e + 1;
    } else {
      hi = s;
    }
  }
  // plain plural/possessive of a known word ("radios", "bob's")
  if (len > 3 && w[len - 1] == 's') {
    int l2 = (w[len - 2] == '\'') ? len - 2 : len - 1;
    w[l2] = 0;
    lo = 0; hi = size;
    while (lo < hi) {
      int mid = (lo + hi) / 2, s = mid;
      while (s > 0 && base[s - 1] != '\n') s--;
      if (s < lo) s = lo;
      int c = cmpLine(base + s, w, l2);
      if (c == 0) return true;
      if (c < 0) { int e = s; while (e < size && base[e] != '\n') e++; lo = e + 1; } else hi = s;
    }
  }
  return false;
}

bool SpellCheck::needsCheck(const char* text, int start, int len) {
  if (len < 2) return false;
  char before = start > 0 ? text[start - 1] : ' ';
  char after = text[start + len];
  // part of a #channel, @mention, number, path, url or email
  if (strchr("#@/\\_-.:", before) && before != 0 && start > 0 && before != '.') return false;
  if (before == '.' && start > 1 && isalnum((unsigned char)text[start - 2])) return false;   // domain.com
  if (isdigit((unsigned char)before) || isdigit((unsigned char)after)) return false;
  if (after == '@' || after == '/' || after == '_' || (after == '.' && isalnum((unsigned char)text[start + len + 1]))) return false;
  // short ALL-CAPS words are acronyms / call signs (GPS, SOS, KJ7ABC)
  bool caps = true;
  for (int i = 0; i < len; i++) if (islower((unsigned char)text[start + i])) { caps = false; break; }
  if (caps && len <= 6) return false;
  return true;
}

bool SpellCheck::addUserWord(const char* word, int len) {
  if (!_user || _nuser >= USER_MAX || len <= 0 || len >= WORD_MAX) return false;
  char w[WORD_MAX];
  for (int i = 0; i < len; i++) w[i] = tolower((unsigned char)word[i]);
  w[len] = 0;
  for (int i = 0; i < _nuser; i++) if (strcmp(_user + i * WORD_MAX, w) == 0) return true;
  strcpy(_user + _nuser++ * WORD_MAX, w);
  if (_fs) {
    File f = _fs->open(USER_FILE, "a", true);
    if (f) { f.print(w); f.print('\n'); f.close(); }
  }
  return true;
}

int SpellCheck::dictionarySize() {
  int n = 0;
  for (const char* p = dict_start; p < dict_end; p++) if (*p == '\n') n++;
  return n;
}
