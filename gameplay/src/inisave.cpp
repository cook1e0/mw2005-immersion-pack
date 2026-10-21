// Settings written to the .ini files off the game thread (see inisave.h).
//
// Each batch is applied one file at a time: the file is read once, every value of the batch is set
// in memory, and the file is written once, to a temporary copy that then replaces it (MoveFileEx).
// One WritePrivateProfileString per setting rewrote the whole file each time: ~300 of them took
// ~1.5 s, and quitting the game in that time lost the rest of the save. Now a save takes a few
// milliseconds, and a quit at any moment leaves either the old file or the new one, never half of
// one. Lines are kept as they are (comments, order, line endings); a value's line becomes
// "key=value", as WritePrivateProfileString writes it.
#include "common.h"
#include "inisave.h"
#include <deque>
#include <stdio.h>
#include <string.h>

namespace {

CRITICAL_SECTION s_lock;
CONDITION_VARIABLE s_wake, s_idle;
std::deque<IniWrite> s_queue;
bool s_busy = false, s_started = false;

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// "[name]" -> name, else empty.
std::string SectionOf(const std::string& line) {
    std::string t = Trim(line);
    return t.size() >= 2 && t.front() == '[' && t.back() == ']' ? Trim(t.substr(1, t.size() - 2)) : std::string();
}

// "key=value" -> key (a comment or a line without '=' -> empty).
std::string KeyOf(const std::string& line) {
    std::string t = Trim(line);
    if (t.empty() || t[0] == ';' || t[0] == '#' || t[0] == '[') return std::string();
    size_t eq = t.find('=');
    return eq == std::string::npos ? std::string() : Trim(t.substr(0, eq));
}

// The lines of the file (without their line ends) and the line end it uses.
bool ReadLines(const std::string& path, std::vector<std::string>* lines, std::string* eol) {
    lines->clear();
    *eol = "\r\n";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return true;   // a new file
    std::string text;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    size_t lf = text.find('\n');
    if (lf != std::string::npos) *eol = lf > 0 && text[lf - 1] == '\r' ? "\r\n" : "\n";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines->push_back(line);
        at = end + 1;
    }
    return true;
}

// Sets key=value in [section] (adding the key at the end of the section, or the section at the end
// of the file); an empty key removes the whole section.
void Apply(std::vector<std::string>& lines, const IniWrite& w) {
    int start = -1, end = (int)lines.size();   // the section's header line, and the next section's
    for (int i = 0; i < (int)lines.size(); i++) {
        std::string s = SectionOf(lines[i]);
        if (s.empty()) continue;
        if (start >= 0) {
            end = i;
            break;
        }
        if (_stricmp(s.c_str(), w.section.c_str()) == 0) start = i;
    }
    if (w.key.empty()) {
        if (start >= 0) lines.erase(lines.begin() + start, lines.begin() + end);
        return;
    }
    std::string line = w.key + "=" + w.value;
    if (start < 0) {
        if (!lines.empty() && !Trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + w.section + "]");
        lines.push_back(line);
        return;
    }
    for (int i = start + 1; i < end; i++)
        if (_stricmp(KeyOf(lines[i]).c_str(), w.key.c_str()) == 0) {
            lines[i] = line;
            return;
        }
    int at = end;   // after the section's last line that isn't blank
    while (at > start + 1 && Trim(lines[at - 1]).empty()) at--;
    lines.insert(lines.begin() + at, line);
}

bool WriteLines(const std::string& path, const std::vector<std::string>& lines, const std::string& eol) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    for (size_t i = 0; i < lines.size(); i++) {
        fwrite(lines[i].data(), 1, lines[i].size(), f);
        if (i + 1 < lines.size() || !lines[i].empty()) fwrite(eol.data(), 1, eol.size(), f);
    }
    bool ok = fflush(f) == 0;
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp.c_str());
        return false;
    }
    return true;
}

// The batch's writes, file by file (in the order the files first appear; each file's writes in
// their own order, so a later value of a key wins).
void WriteBatch(const std::deque<IniWrite>& batch) {
    std::vector<std::string> files;
    for (const IniWrite& w : batch) {
        bool seen = false;
        for (const std::string& f : files) seen |= _stricmp(f.c_str(), w.file.c_str()) == 0;
        if (!seen) files.push_back(w.file);
    }
    for (const std::string& file : files) {
        std::vector<std::string> lines;
        std::string eol;
        ReadLines(file, &lines, &eol);
        int n = 0;
        for (const IniWrite& w : batch)
            if (_stricmp(w.file.c_str(), file.c_str()) == 0) Apply(lines, w), n++;
        if (WriteLines(file, lines, eol)) continue;
        // Couldn't replace the file (locked?): the old way, one value at a time.
        Log("ui: %s could not be written in one go, %d settings written one by one", file.c_str(), n);
        for (const IniWrite& w : batch)
            if (_stricmp(w.file.c_str(), file.c_str()) == 0)
                WritePrivateProfileStringA(w.section.c_str(), w.key.empty() ? nullptr : w.key.c_str(), w.value.c_str(),
                                           w.file.c_str());
    }
}

DWORD WINAPI Writer(LPVOID) {
    EnterCriticalSection(&s_lock);
    for (;;) {
        while (s_queue.empty()) {
            s_busy = false;
            WakeAllConditionVariable(&s_idle);
            SleepConditionVariableCS(&s_wake, &s_lock, INFINITE);
        }
        s_busy = true;
        std::deque<IniWrite> batch;
        batch.swap(s_queue);
        LeaveCriticalSection(&s_lock);
        double t0 = Now();
        WriteBatch(batch);
        Log("ui: %u settings written in the background (%.0f ms)", (unsigned)batch.size(), (Now() - t0) * 1000.0);
        EnterCriticalSection(&s_lock);
    }
}

void Start() {
    if (s_started) return;
    s_started = true;
    InitializeCriticalSection(&s_lock);
    InitializeConditionVariable(&s_wake);
    InitializeConditionVariable(&s_idle);
    HANDLE t = CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
        CloseHandle(t);
    }
}

}  // namespace

void IniSave_Queue(const std::vector<IniWrite>& writes) {
    Start();
    EnterCriticalSection(&s_lock);
    // Every value, changed or not: other code edits these files too (a car's own physics section
    // is removed when it goes back to the global values), so "unchanged since the last save" says
    // nothing about the file.
    for (const IniWrite& w : writes) s_queue.push_back(w);
    if (!writes.empty()) WakeConditionVariable(&s_wake);
    LeaveCriticalSection(&s_lock);
}

void IniSave_Flush() {
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    while (s_busy || !s_queue.empty()) SleepConditionVariableCS(&s_idle, &s_lock, 2000);
    LeaveCriticalSection(&s_lock);
}
