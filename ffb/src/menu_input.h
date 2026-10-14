// Neutral values use the game's data format and axis ranges, not a pedal's rest position:
// a released pedal at an axis endpoint would still mean "held direction" in a menu.
#pragma once
#include "common.h"
#include <string.h>
#include <vector>

struct MenuObject {
    DWORD offset, type, neutral;
    bool Axis() const { return (type & DIDFT_AXIS) != 0; }
    DWORD Size() const { return (type & DIDFT_BUTTON) ? 1 : sizeof(DWORD); }
};

inline DWORD MenuAxisCenter(LONG lo, LONG hi) {
    return (DWORD)((int64_t)lo + ((int64_t)hi - lo) / 2);
}

inline bool MenuAxisRelative(DWORD type, bool relativeFormat) {
    // DIDFT_AXIS (3) means either axis type in an optional/wildcard data-format entry.
    return relativeFormat || (type & DIDFT_AXIS) == DIDFT_RELAXIS;
}

inline void NeutralizeMenuState(void* state, DWORD size, const std::vector<MenuObject>& objects, bool all) {
    for (const auto& o : objects) {
        if ((!all && !o.Axis()) || o.offset > size || o.Size() > size - o.offset) continue;
        memcpy((uint8_t*)state + o.offset, &o.neutral, o.Size());
    }
}

// Replace axis events with centre events rather than discarding them: this releases an axis
// held during the drive when a pause menu opens. Buttons/hats and event metadata stay intact.
// Same transformation for DIGDD_PEEK; the real device retains ownership of its queue.
inline void NeutralizeMenuEvents(DWORD stride, void* events, DWORD count,
                                 const std::vector<MenuObject>& objects, bool all) {
    if (!events || stride < offsetof(DIDEVICEOBJECTDATA, dwData) + sizeof(DWORD)) return;
    for (DWORD i = 0; i < count; ++i) {
        uint8_t* e = (uint8_t*)events + (size_t)i * stride;
        DWORD offset;
        memcpy(&offset, e + offsetof(DIDEVICEOBJECTDATA, dwOfs), sizeof(offset));
        for (const auto& o : objects) {
            if (o.offset != offset || (!all && !o.Axis())) continue;
            memcpy(e + offsetof(DIDEVICEOBJECTDATA, dwData), &o.neutral, sizeof(o.neutral));
            break;
        }
    }
}

struct MenuResetQueue {
    int mode = -1;              // 0 = pass through, 1 = axes centred, 2 = all input neutral
    std::vector<DIDEVICEOBJECTDATA> pending;

    void Reset() { mode = -1; pending.clear(); }

    // Refresh deferred snapshots from the current state. Buffered events can still
    // contain a backlog older than the snapshot, including events from the pause.
    void Refresh(const void* state, DWORD size, const std::vector<MenuObject>& objects) {
        for (auto& e : pending) {
            for (const auto& o : objects) {
                if (o.offset != e.dwOfs || o.offset > size || o.Size() > size - o.offset) continue;
                e.dwData = 0;
                memcpy(&e.dwData, (const uint8_t*)state + o.offset, o.Size());
                if (mode == 2 || (mode == 1 && o.Axis())) e.dwData = o.neutral;
                break;
            }
        }
    }

    // A current snapshot is redundant if the last real event already reports it.
    // PEEK leaves that event in the driver queue, so invalidation is safe there too.
    void DiscardSuperseded(DWORD stride, const void* events, DWORD count) {
        if (!events || stride < 16) return;
        for (auto p = pending.begin(); p != pending.end();) {
            bool found = false;
            DWORD last = 0;
            for (DWORD i = 0; i < count; ++i) {
                const uint8_t* event = (const uint8_t*)events + (size_t)i * stride;
                DWORD offset;
                memcpy(&offset, event, sizeof(offset));
                if (offset != p->dwOfs) continue;
                memcpy(&last, event + offsetof(DIDEVICEOBJECTDATA, dwData), sizeof(last));
                found = true;
            }
            if (found && last == p->dwData) p = pending.erase(p);
            else ++p;
        }
    }

    void Change(int next, const void* state, DWORD size, const std::vector<MenuObject>& objects) {
        if (next == mode) return;
        int previous = mode;
        mode = next;
        pending.clear();
        if (previous == -1 && next == 0) return;
        for (const auto& o : objects) {
            // Only resync objects affected by the filter, to avoid replaying a held menu button.
            if (next != 2 && previous != 2 && !o.Axis()) continue;
            if (o.offset > size || o.Size() > size - o.offset) continue;
            DIDEVICEOBJECTDATA e = {};
            e.dwOfs = o.offset;
            // Snapshot on transitions also restores stationary driving controls on resume.
            memcpy(&e.dwData, (const uint8_t*)state + o.offset, o.Size());
            if (next == 2 || (next == 1 && o.Axis())) e.dwData = o.neutral;
            e.dwTimeStamp = GetTickCount();
            pending.push_back(e);
        }
    }

    DWORD Append(DWORD stride, void* events, DWORD count, DWORD capacity, bool peek) {
        if (stride < 16 || count > capacity) return count;
        DWORD n = (DWORD)pending.size();
        if (n > capacity - count) n = capacity - count;
        if (events) {
            for (DWORD i = 0; i < n; i++) {
                void* dest = (uint8_t*)events + (size_t)(count + i) * stride;
                memset(dest, 0, stride);
                memcpy(dest, &pending[i], stride < sizeof(pending[i]) ? stride : sizeof(pending[i]));
            }
        }
        if (!peek) pending.erase(pending.begin(), pending.begin() + n);
        return count + n;
    }
};
