#pragma once
// maxmesh storage: LittleFS on the "spiffs" data partition, with a one-time, power-cut-safe migration of
// the files an older firmware left in SPIFFS (contacts, channels, identity, prefs, messages, UI settings).
class DisplayDriver;

// Mount LittleFS, migrating old SPIFFS data first if needed. Returns false only if no file system
// could be mounted at all.
bool mxStorageBegin(DisplayDriver* disp);

// What happened at boot (for the status console): 0 = already LittleFS, 1 = migrated from SPIFFS,
// 2 = resumed an interrupted migration, 3 = fresh (formatted), -1 = failed. Files/bytes moved.
int mxStorageMigration(int* files = nullptr, int* bytes = nullptr);

// Remove leftovers of an old storage layout, one small step per call (call while locked/idle).
// Returns true while there is more to do.
bool mxStorageIdleCleanup();
