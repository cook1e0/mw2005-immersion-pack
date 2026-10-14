#include "../src/menu_input.h"
#include <assert.h>
#include <stdio.h>

int main() {
    assert(MenuAxisCenter(0, 65535) == 32767);
    assert((LONG)MenuAxisCenter(-1000, 1000) == 0);
    assert((LONG)MenuAxisCenter(-2000, -1000) == -1500);
    assert((LONG)MenuAxisCenter(INT32_MIN, INT32_MAX) == -1);
    assert(!MenuAxisRelative(DIDFT_AXIS | DIDFT_OPTIONAL, false));
    assert(!MenuAxisRelative(DIDFT_ABSAXIS, false));
    assert(MenuAxisRelative(DIDFT_RELAXIS, false));
    assert(MenuAxisRelative(DIDFT_AXIS, true));
    std::vector<MenuObject> objects = {
        {DIJOFS_X, DIDFT_ABSAXIS, MenuAxisCenter(-1000, 1000)},
        {DIJOFS_Y, DIDFT_ABSAXIS, MenuAxisCenter(0, 65535)},
        {DIJOFS_Z, DIDFT_RELAXIS, 0},
        {DIJOFS_POV(0), DIDFT_POV, 0xFFFFFFFFu},
        {DIJOFS_BUTTON(0), DIDFT_PSHBUTTON, 0},
    };
    DIJOYSTATE js = {};
    js.lX = -1000; js.lY = 65535; js.lZ = 99;
    js.rgdwPOV[0] = 9000; js.rgbButtons[0] = 0x80;
    NeutralizeMenuState(&js, sizeof(js), objects, false);
    assert(js.lX == 0 && js.lY == 32767 && js.lZ == 0);
    assert(js.rgdwPOV[0] == 9000 && js.rgbButtons[0] == 0x80);
    NeutralizeMenuState(&js, sizeof(js), objects, true);
    assert(js.rgdwPOV[0] == 0xFFFFFFFF && js.rgbButtons[0] == 0);

    // Nonstandard formats, byte buttons, and a caller whose state buffer is smaller.
    std::vector<MenuObject> custom = {{3, DIDFT_ABSAXIS, 123}, {9, DIDFT_PSHBUTTON, 0},
                                     {10, DIDFT_POV, 0xFFFFFFFFu}, {0xFFFFFFFFu, DIDFT_ABSAXIS, 0}};
    uint8_t bytes[11]; memset(bytes, 0xAB, sizeof(bytes));
    NeutralizeMenuState(bytes, 10, custom, true);
    DWORD value; memcpy(&value, bytes + 3, 4);
    assert(value == 123 && bytes[9] == 0 && bytes[10] == 0xAB && bytes[2] == 0xAB && bytes[7] == 0xAB);

    DIDEVICEOBJECTDATA events[3] = {};
    events[0].dwOfs = DIJOFS_Y; events[0].dwData = 65535;
    events[0].dwTimeStamp = 42; events[0].dwSequence = 123; events[0].uAppData = 999;
    events[1].dwOfs = DIJOFS_BUTTON(0); events[1].dwData = 0x80;
    events[2].dwOfs = DIJOFS_POV(0); events[2].dwData = 9000;
    NeutralizeMenuEvents(sizeof(events[0]), events, 3, objects, false);
    assert(events[0].dwData == 32767 && events[0].dwTimeStamp == 42 && events[0].dwSequence == 123 && events[0].uAppData == 999);
    assert(events[1].dwData == 0x80 && events[2].dwData == 9000);
    NeutralizeMenuEvents(sizeof(events[0]), events, 3, objects, true);
    assert(events[1].dwData == 0 && events[2].dwData == 0xFFFFFFFF);
    // Older DirectInput event format (no uAppData), as well as count-only / short records.
    DWORD oldEvents[2][4] = {{DIJOFS_X, 1000, 77, 88}, {DIJOFS_Y, 65535, 99, 100}};
    NeutralizeMenuEvents(16, oldEvents, 2, objects, false);
    assert(oldEvents[0][1] == 0 && oldEvents[1][1] == 32767 && oldEvents[0][2] == 77 && oldEvents[1][3] == 100);
    NeutralizeMenuEvents(16, nullptr, INFINITE, objects, true);
    NeutralizeMenuEvents(4, oldEvents, 2, objects, true);

    // A stationary endpoint needs a release event on menu entry, even if the real queue is empty.
    js.lY = 65535; js.rgbButtons[0] = 0x80; js.rgdwPOV[0] = 9000;
    MenuResetQueue reset;
    reset.Change(1, &js, sizeof(js), objects);
    DIDEVICEOBJECTDATA resetEvents[8] = {};
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 1, true) == 1);
    assert(reset.pending.size() == 3);  // PEEK must not consume the synthetic queue
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 8, false) == 3);
    assert(resetEvents[1].dwData == 32767);
    assert(reset.pending.empty());
    reset.Change(1, &js, sizeof(js), objects);
    assert(reset.pending.empty());    // no repeated snapshots while the same menu stays open
    reset.Change(0, &js, sizeof(js), objects);
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 8, false) == 3);
    assert(resetEvents[1].dwData == 65535); // restore a stationary pedal when driving resumes
    reset.Change(2, &js, sizeof(js), objects);
    assert(reset.Append(16, nullptr, 0, 2, true) == 2 && reset.pending.size() == 5);
    assert(reset.Append(16, nullptr, 0, INFINITE, false) == 5 && reset.pending.empty());
    reset.Reset();
    assert(reset.mode == -1);

    // Resume with a held pedal, then release it while the synthetic queue is still pending.
    reset.Change(1, &js, sizeof(js), objects);
    reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 8, false);
    reset.Change(0, &js, sizeof(js), objects);
    resetEvents[0].dwOfs = DIJOFS_Y;
    resetEvents[0].dwData = 0;
    js.lY = 0;
    reset.Refresh(&js, sizeof(js), objects);
    reset.DiscardSuperseded(sizeof(resetEvents[0]), resetEvents, 1);
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 1, 1, false) == 1);
    assert(resetEvents[0].dwData == 0);
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 8, false) == 2);
    for (int i = 0; i < 2; ++i) assert(resetEvents[i].dwOfs != DIJOFS_Y);

    // PEEK invalidates old snapshots without consuming unaffected snapshots or real events.
    reset.Change(1, &js, sizeof(js), objects);
    reset.Change(0, &js, sizeof(js), objects);
    DWORD released[1][4] = {{DIJOFS_Y, 0, 42, 43}};
    reset.DiscardSuperseded(16, released, 1);
    assert(reset.pending.size() == 2 && released[0][1] == 0);
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 8, true) == 2);
    assert(reset.pending.size() == 2);
    reset.DiscardSuperseded(16, nullptr, INFINITE);

    // Old events in the driver's backlog cannot replace a newer snapshot.
    js.lY = 65535;
    reset.Change(1, &js, sizeof(js), objects);
    reset.Change(0, &js, sizeof(js), objects);
    reset.Refresh(&js, sizeof(js), objects);
    reset.DiscardSuperseded(16, released, 1);
    assert(reset.pending.size() == 3);
    // Compare the last event for each object, rather than an earlier matching event.
    DWORD backlog[2][4] = {{DIJOFS_Y, 65535, 1, 1}, {DIJOFS_Y, 0, 2, 2}};
    reset.DiscardSuperseded(16, backlog, 2);
    assert(reset.pending.size() == 3);
    assert(reset.Append(sizeof(resetEvents[0]), resetEvents, 0, 8, false) == 3);
    assert(resetEvents[1].dwData == 65535);
    puts("menu input tests passed");
}
