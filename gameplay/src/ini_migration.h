// Recognise the shipped gameplay defaults before replacing them with legacy settings.
// Unknown keys or edited values make a file user-owned; it must be preserved.
#pragma once
#include "common.h"
#include <ctype.h>
#include <string.h>

inline std::string IniTrim(std::string text) {
    size_t comment = text.find(';');
    if (comment != std::string::npos) text.resize(comment);
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

inline uint64_t IniFingerprint(const char* path) {
    char sections[32768], entries[32768];
    DWORD n = GetPrivateProfileSectionNamesA(sections, sizeof(sections), path);
    if (!n || n >= sizeof(sections) - 2) return 0;
    std::vector<std::string> rows;
    for (const char* s = sections; *s; s += strlen(s) + 1) {
        std::string section = IniTrim(s);
        for (char& c : section) c = (char)tolower((unsigned char)c);
        DWORD m = GetPrivateProfileSectionA(s, entries, sizeof(entries), path);
        if (!m || m >= sizeof(entries) - 2) return 0;
        for (const char* e = entries; *e; e += strlen(e) + 1) {
            std::string entry = e;
            size_t equals = entry.find('=');
            if (equals == std::string::npos) return 0;
            std::string key = IniTrim(entry.substr(0, equals));
            for (char& c : key) c = (char)tolower((unsigned char)c);
            rows.push_back(section + "/" + key + "=" + IniTrim(entry.substr(equals + 1)));
        }
    }
    std::sort(rows.begin(), rows.end());
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const std::string& row : rows) {
        for (unsigned char c : row) hash = (hash ^ c) * UINT64_C(1099511628211);
        hash = (hash ^ '\n') * UINT64_C(1099511628211);
    }
    return hash;
}

// Update with nfsmw_gameplay.ini; the gameplay regression tests check for drift.
constexpr uint64_t kGameplayDefaultIniFingerprint = UINT64_C(0x5980298e11fae933);

inline bool MigrateLegacyIni(const char* legacy, const char* current) {
    if (GetFileAttributesA(legacy) == INVALID_FILE_ATTRIBUTES) return false;
    bool exists = GetFileAttributesA(current) != INVALID_FILE_ATTRIBUTES;
    if (exists) {
        if (IniFingerprint(current) != kGameplayDefaultIniFingerprint) {
            Log("settings: keeping customised gameplay ini; legacy shifter ini retained");
            return false;
        }
        std::string backup = std::string(current) + ".pre-hshifter.bak";
        if (!CopyFileA(current, backup.c_str(), TRUE)) {
            Log("settings: could not back up gameplay defaults; legacy ini retained (error %lu)", GetLastError());
            return false;
        }
    }
    // Flush the profile cache before replacing a file that was just inspected.
    WritePrivateProfileStringA(nullptr, nullptr, nullptr, current);
    if (!MoveFileExA(legacy, current, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        Log("settings: could not migrate legacy ini (error %lu)", GetLastError());
        return false;
    }
    Log("settings: nfsmw_hshifter.ini migrated to nfsmw_gameplay.ini");
    return true;
}
