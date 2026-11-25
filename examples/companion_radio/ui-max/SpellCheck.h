#pragma once

// Tiny spell checker for the T-Deck MAX UI.
// The word list (spell_words.txt: lowercase, one per line, byte-sorted; built by
// tools/make_dict.py from SCOWL size 50 US) is embedded in flash and binary-searched
// in place. A personal dictionary lives in SPIFFS (/mx_words.txt).

#include <Arduino.h>
#include <FS.h>

class SpellCheck {
  fs::FS* _fs = nullptr;
  char* _user = nullptr;      // personal words, 24 chars each
  int _nuser = 0;
public:
  static const int USER_MAX = 400;
  static const int WORD_MAX = 24;

  void begin(fs::FS& fs);
  bool known(const char* word, int len);         // case-insensitive
  bool needsCheck(const char* text, int start, int len);   // skip numbers, #tags, @names, ACRONYMS
  bool addUserWord(const char* word, int len);
  int  dictionarySize();                          // number of embedded words (approx, for diagnostics)
};

extern SpellCheck spell;
