// hshiftertool: find and configure the clutch pedal and H-shifter for nfsmw_hshifter.
//   hshiftertool list              attached controllers
//   hshiftertool monitor           live axes / buttons / hats (Esc to quit)
//   hshiftertool setup [ini]       detect clutch and shifter, write the ini
#include "../src/devices.h"
#include <conio.h>
#include <stdio.h>
#include <math.h>

static HWND s_hwnd;

static std::vector<JoyDevice> OpenAll() {
    std::vector<JoyDevice> devs = DI_Enumerate();
    for (auto& d : devs) {
        if (!DI_Open(d, s_hwnd)) printf("  (could not open \"%s\")\n", d.name.c_str());
    }
    return devs;
}

static void ReadAll(std::vector<JoyDevice>& devs) {
    for (auto& d : devs) DI_Read(d);
}

static void Describe(int i, const JoyDevice& d) {
    printf("joy%d  %s\n      axes:", i, d.name.c_str());
    for (int a = 0; a < 8; a++)
        if (d.hasAxis[a]) printf(" %s", kAxisNames[a]);
    printf("   buttons: %d   hats: %d\n", d.numButtons, d.numPovs);
}

static int CmdList() {
    auto devs = OpenAll();
    if (devs.empty()) printf("No game controllers attached.\n");
    for (size_t i = 0; i < devs.size(); i++) Describe((int)i, devs[i]);
    printf("\njoyN numbers follow this order unless [Devices] JoyN= names a device.\n");
    return 0;
}

static int CmdMonitor() {
    auto devs = OpenAll();
    if (devs.empty()) {
        printf("No game controllers attached.\n");
        return 1;
    }
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(out, &info);
    COORD home = info.dwCursorPosition;
    printf("\n");
    for (;;) {
        if (_kbhit() && _getch() == 27) break;
        ReadAll(devs);
        SetConsoleCursorPosition(out, home);
        printf("Esc to quit\n");
        for (size_t i = 0; i < devs.size(); i++) {
            const JoyDevice& d = devs[i];
            printf("joy%-2u %-60.60s\n  ", (unsigned)i, d.name.c_str());
            for (int a = 0; a < 8; a++)
                if (d.hasAxis[a]) printf("%s=%-6ld ", kAxisNames[a], d.state.axis[a]);
            printf("%-20s\n  buttons:", "");
            int shown = 0;
            for (int b = 0; b < 128; b++)
                if (d.state.btn[b] & 0x80) shown += printf(" b%d", b);
            for (int p = 0; p < d.numPovs; p++)
                if (LOWORD(d.state.pov[p]) != 0xFFFF) shown += printf(" pov%d=%lu", p, d.state.pov[p] / 100);
            printf("%*s\n", std::max(0, 60 - shown), "");
        }
        Sleep(30);
    }
    return 0;
}

// --- setup wizard -------------------------------------------------------------------------
static int WaitKey() {   // returns 'enter', 's' or 'q'
    for (;;) {
        int c = _getch();
        if (c == '\r') return '\r';
        c = tolower(c);
        if (c == 's' || c == 'q') return c;
    }
}

static std::vector<JoyState> Sample(std::vector<JoyDevice>& devs) {
    // average axes over a few reads, OR buttons
    std::vector<JoyState> acc(devs.size());
    std::vector<double> sum(devs.size() * 8, 0.0);
    const int n = 10;
    for (int k = 0; k < n; k++) {
        ReadAll(devs);
        for (size_t i = 0; i < devs.size(); i++) {
            for (int a = 0; a < 8; a++) sum[i * 8 + a] += devs[i].state.axis[a];
            for (int b = 0; b < 128; b++) acc[i].btn[b] |= devs[i].state.btn[b];
            memcpy(acc[i].pov, devs[i].state.pov, sizeof(acc[i].pov));
        }
        Sleep(10);
    }
    for (size_t i = 0; i < devs.size(); i++)
        for (int a = 0; a < 8; a++) acc[i].axis[a] = (LONG)lround(sum[i * 8 + a] / n);
    return acc;
}

struct Found {
    int dev = -1;
    Binding bind;
};

// A button or hat direction that is active in `now` but not in `base`.
static Found NewPress(std::vector<JoyDevice>& devs, const std::vector<JoyState>& base, const std::vector<JoyState>& now) {
    Found f;
    int count = 0;
    for (size_t i = 0; i < devs.size(); i++) {
        for (int b = 0; b < 128; b++)
            if ((now[i].btn[b] & 0x80) && !(base[i].btn[b] & 0x80)) {
                if (count++ == 0) {
                    f.dev = (int)i;
                    f.bind.kind = BK_BUTTON;
                    f.bind.index = b;
                }
            }
        for (int p = 0; p < devs[i].numPovs; p++)
            for (int dir = 0; dir < 4; dir++)
                if (PovMatches(now[i].pov[p], dir) && !PovMatches(base[i].pov[p], dir)) {
                    if (count++ == 0) {
                        f.dev = (int)i;
                        f.bind.kind = BK_POV;
                        f.bind.index = p;
                        f.bind.povDir = dir;
                    }
                }
    }
    if (count > 1) printf("  (several inputs changed, using the first)\n");
    return f;
}

static int CmdSetup(const char* ini) {
    auto devs = OpenAll();
    if (devs.empty()) {
        printf("No game controllers attached. Plug in your pedals / shifter and try again.\n");
        return 1;
    }
    printf("Controllers:\n");
    for (size_t i = 0; i < devs.size(); i++) Describe((int)i, devs[i]);

    // device index -> joy slot written to the ini; keep slots already named there so bindings
    // from a section that gets skipped stay valid
    std::vector<int> slot(devs.size(), -1);
    bool taken[kMaxJoys] = {};
    for (int j = 0; j < kMaxJoys; j++) {
        char key[16], name[260];
        snprintf(key, sizeof(key), "Joy%d", j);
        GetPrivateProfileStringA("Devices", key, "", name, sizeof(name), ini);
        if (!name[0]) continue;
        taken[j] = true;
        for (size_t i = 0; i < devs.size(); i++)
            if (slot[i] < 0 && _stricmp(devs[i].name.c_str(), name) == 0) {
                slot[i] = j;
                break;
            }
    }
    auto slotFor = [&](int dev) {
        if (slot[dev] < 0) {
            int j = 0;
            while (j < kMaxJoys - 1 && taken[j]) j++;
            taken[j] = true;
            slot[dev] = j;
        }
        return slot[dev];
    };

    // Clutch
    std::string clutch;
    int released = -1, pressed = -1;
    printf("\n== Clutch ==\nRelease all pedals, then press ENTER (S to skip the clutch).\n");
    if (WaitKey() == '\r') {
        auto up = Sample(devs);
        printf("Now press the CLUTCH pedal all the way down, hold it and press ENTER.\n");
        WaitKey();
        auto down = Sample(devs);
        int bestDev = -1, bestAxis = -1;
        long bestDelta = 0;
        for (size_t i = 0; i < devs.size(); i++)
            for (int a = 0; a < 8; a++) {
                if (!devs[i].hasAxis[a]) continue;
                long d = labs(down[i].axis[a] - up[i].axis[a]);
                if (d > bestDelta) {
                    bestDelta = d;
                    bestDev = (int)i;
                    bestAxis = a;
                }
            }
        if (bestDelta > 12000) {
            Binding b;
            b.kind = BK_AXIS;
            b.joy = slotFor(bestDev);
            b.index = bestAxis;
            clutch = BindingName(b);
            released = up[bestDev].axis[bestAxis];
            pressed = down[bestDev].axis[bestAxis];
            printf("  clutch = %s on \"%s\" (released %d, pressed %d)\n", clutch.c_str(),
                   devs[bestDev].name.c_str(), released, pressed);
        } else {
            Found f = NewPress(devs, up, down);
            if (f.dev >= 0) {
                f.bind.joy = slotFor(f.dev);
                clutch = BindingName(f.bind);
                printf("  clutch = %s (button: the clutch is ramped in and out)\n", clutch.c_str());
            } else {
                printf("  nothing moved - clutch left unchanged.\n");
            }
        }
    }

    // Shifter
    std::string gears[kMaxGears + 1];
    bool anyGear = false;
    printf("\n== H-shifter ==\nPut the shifter in NEUTRAL and press ENTER (S to skip the shifter).\n");
    if (WaitKey() == '\r') {
        auto neutral = Sample(devs);
        const char* order[] = {"1", "2", "3", "4", "5", "6", "7", "R"};
        for (int k = 0; k < 8; k++) {
            int g = order[k][0] == 'R' ? 0 : k + 1;
            for (;;) {
                printf("Put it in gear %s and press ENTER  (S = this gear doesn't exist, Q = done)\n", order[k]);
                int key = WaitKey();
                if (key == 'q') goto shifterDone;
                if (key == 's') break;
                auto now = Sample(devs);
                Found f = NewPress(devs, neutral, now);
                if (f.dev < 0) {
                    printf("  nothing detected, try again.\n");
                    continue;
                }
                f.bind.joy = slotFor(f.dev);
                gears[g] = BindingName(f.bind);
                anyGear = true;
                printf("  gear %s = %s\n", order[k], gears[g].c_str());
                break;
            }
        }
    }
shifterDone:

    if (clutch.empty() && !anyGear) {
        printf("\nNothing to save.\n");
        return 0;
    }
    char buf[32];
    for (size_t i = 0; i < devs.size(); i++) {
        if (slot[i] < 0) continue;
        snprintf(buf, sizeof(buf), "Joy%d", slot[i]);
        WritePrivateProfileStringA("Devices", buf, devs[i].name.c_str(), ini);
    }
    if (!clutch.empty()) {
        WritePrivateProfileStringA("Clutch", "Clutch", clutch.c_str(), ini);
        snprintf(buf, sizeof(buf), "%d", released);
        WritePrivateProfileStringA("Clutch", "Released", released >= 0 ? buf : "-1", ini);
        snprintf(buf, sizeof(buf), "%d", pressed);
        WritePrivateProfileStringA("Clutch", "Pressed", pressed >= 0 ? buf : "-1", ini);
    }
    if (anyGear) {
        WritePrivateProfileStringA("Shifter", "Reverse", gears[0].c_str(), ini);
        for (int g = 1; g <= kMaxGears; g++) {
            snprintf(buf, sizeof(buf), "Gear%d", g);
            WritePrivateProfileStringA("Shifter", buf, gears[g].c_str(), ini);
        }
    }
    printf("\nSaved to %s\n", ini);
    return 0;
}

int main(int argc, char** argv) {
    if (!DI_Init()) {
        printf("DirectInput is not available.\n");
        return 1;
    }
    s_hwnd = GetConsoleWindow();
    if (!s_hwnd) s_hwnd = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    const char* cmd = argc > 1 ? argv[1] : "";
    if (!_stricmp(cmd, "list")) return CmdList();
    if (!_stricmp(cmd, "monitor")) return CmdMonitor();
    if (!_stricmp(cmd, "setup")) {
        char ini[MAX_PATH];
        if (argc > 2) {
            GetFullPathNameA(argv[2], MAX_PATH, ini, nullptr);
        } else {
            GetModuleFileNameA(nullptr, ini, MAX_PATH);
            char* slash = strrchr(ini, '\\');
            strcpy(slash ? slash + 1 : ini, "nfsmw_hshifter.ini");
        }
        return CmdSetup(ini);
    }
    printf("usage:\n"
           "  hshiftertool list            attached controllers and their inputs\n"
           "  hshiftertool monitor         live view of every axis, button and hat\n"
           "  hshiftertool setup [ini]     detect clutch pedal and H-shifter, write nfsmw_hshifter.ini\n"
           "                               (default: the ini next to this exe)\n");
    return 1;
}
