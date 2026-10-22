// Settings written to the .ini files off the game thread: WritePrivateProfileString rewrites the
// whole file on every call, so the F4 menu's save (one call per setting, ~300) froze the game for
// about half a second.
#pragma once
#include <string>
#include <vector>

struct IniWrite {
    std::string file, section, key, value;   // an empty key deletes the whole section
    bool erase = false;                      // removes the key (a setting that is gone or renamed)
};

// A write that removes `key` from [section].
inline IniWrite IniErase(const std::string& file, const std::string& section, const std::string& key) {
    IniWrite w{file, section, key, ""};
    w.erase = true;
    return w;
}

// Queues the values and returns at once; a worker thread writes them in order, one batch after
// another, so a later save always lands after an earlier one. Game thread only.
void IniSave_Queue(const std::vector<IniWrite>& writes);

// Blocks until everything queued is written (at exit). Any thread.
void IniSave_Flush();
