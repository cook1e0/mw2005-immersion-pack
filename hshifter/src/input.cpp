#include "input.h"
#include "devices.h"
#include <math.h>
#include <mmsystem.h>

static CRITICAL_SECTION s_lock;
static InputSnapshot s_snap;
static volatile LONG s_started;

static std::vector<Binding> s_clutch, s_gear[kMaxGears + 1], s_neutral, s_toggle, s_blow, s_repair, s_menu, s_nav[4];
static JoyDevice s_joy[kMaxJoys];
static bool s_joyWanted[kMaxJoys];
static double s_joyOpened[kMaxJoys];

// Per clutch-axis binding: raw values for pedal up / floored (learned when not configured).
struct AxisCal {
    bool known = false;
    float released = 0, pressed = 65535;
};
static std::vector<AxisCal> s_cal;

static HWND CreateHiddenWindow() {
    return CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "nfsmw_hshifter", WS_POPUP, 0, 0, 1, 1,
                           nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
}

static bool GameHasFocus() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void ParseAll() {
    std::string err;
    auto parse = [&](const char* what, const std::string& text, std::vector<Binding>& out) {
        if (!ParseBindings(text, out, &err)) Log("config: %s: %s", what, err.c_str());
        for (auto& b : out)
            if (b.kind != BK_KEY) s_joyWanted[b.joy] = true;
    };
    parse("Clutch", g_cfg.clutch, s_clutch);
    parse("Reverse", g_cfg.gear[0], s_gear[0]);
    for (int g = 1; g <= kMaxGears; g++) {
        char name[16];
        snprintf(name, sizeof(name), "Gear%d", g);
        parse(name, g_cfg.gear[g], s_gear[g]);
    }
    parse("Neutral", g_cfg.neutral, s_neutral);
    parse("Toggle", g_cfg.toggle, s_toggle);
    parse("DebugBlow", g_cfg.debugBlow, s_blow);
    parse("DebugRepair", g_cfg.debugRepair, s_repair);
    parse("Menu", g_cfg.menu, s_menu);
    parse("MenuUp", g_cfg.menuUp, s_nav[0]);
    parse("MenuDown", g_cfg.menuDown, s_nav[1]);
    parse("MenuLeft", g_cfg.menuLeft, s_nav[2]);
    parse("MenuRight", g_cfg.menuRight, s_nav[3]);

    s_cal.resize(s_clutch.size());
    for (size_t i = 0; i < s_clutch.size(); i++) {
        if (s_clutch[i].kind != BK_AXIS) continue;
        if (g_cfg.clutchReleased >= 0) {
            s_cal[i].known = true;
            s_cal[i].released = (float)g_cfg.clutchReleased;
            s_cal[i].pressed = g_cfg.clutchPressed >= 0 ? (float)g_cfg.clutchPressed
                                                        : (g_cfg.clutchReleased > 32767 ? 0.0f : 65535.0f);
        } else if (s_clutch[i].invert) {
            s_cal[i] = {true, 65535.0f, 0.0f};
        }
    }
}

// Assigns JoyN slots to attached devices: by name substring if configured, else enumeration order.
static void OpenDevices(HWND hwnd) {
    std::vector<JoyDevice> attached = DI_Enumerate();
    std::vector<bool> used(attached.size(), false);
    for (int j = 0; j < kMaxJoys; j++)
        if (s_joy[j].dev)
            for (size_t k = 0; k < attached.size(); k++)
                if (attached[k].guid == s_joy[j].guid) used[k] = true;

    for (int j = 0; j < kMaxJoys; j++) {
        if (!s_joyWanted[j] || s_joy[j].dev) continue;
        int pick = -1;
        if (!g_cfg.joyName[j].empty()) {
            std::string want = g_cfg.joyName[j];
            for (auto& c : want) c = (char)tolower((unsigned char)c);
            for (size_t k = 0; k < attached.size() && pick < 0; k++) {
                std::string n = attached[k].name;
                for (auto& c : n) c = (char)tolower((unsigned char)c);
                if (!used[k] && n.find(want) != std::string::npos) pick = (int)k;
            }
        } else if (j < (int)attached.size() && !used[j]) {
            pick = j;
        }
        if (pick < 0) continue;
        JoyDevice d = attached[pick];
        if (DI_Open(d, hwnd)) {
            used[pick] = true;
            s_joy[j] = d;
            s_joyOpened[j] = Now();
            Log("input: joy%d = \"%s\" (%d buttons, %d hats)", j, d.name.c_str(), d.numButtons, d.numPovs);
        }
    }
}

static bool BindingUsable(const Binding& b) {
    return b.kind == BK_KEY || (s_joy[b.joy].dev && s_joy[b.joy].ok);
}

static bool Down(const Binding& b, bool focus) {
    switch (b.kind) {
        case BK_KEY: return focus && (GetAsyncKeyState(b.index) & 0x8000);
        case BK_BUTTON: return s_joy[b.joy].ok && (s_joy[b.joy].state.btn[b.index] & 0x80);
        case BK_POV: return s_joy[b.joy].ok && PovMatches(s_joy[b.joy].state.pov[b.index], b.povDir);
        case BK_AXIS: return s_joy[b.joy].ok && fabsf(s_joy[b.joy].state.axis[b.index] - 32767.5f) > 24000.0f;
    }
    return false;
}

static bool AnyDown(const std::vector<Binding>& list, bool focus) {
    for (auto& b : list)
        if (Down(b, focus)) return true;
    return false;
}

static bool AnyUsable(const std::vector<Binding>& list) {
    for (auto& b : list)
        if (BindingUsable(b)) return true;
    return false;
}

// Counts presses of a held control, repeating after a delay like a keyboard.
struct Repeater {
    bool prev = false;
    double next = 0;
    uint32_t Update(bool down, double now) {
        uint32_t n = 0;
        if (down && !prev) {
            n = 1;
            next = now + 0.35;
        } else if (down && now >= next) {
            n = 1;
            next = now + 0.07;
        }
        prev = down;
        return n;
    }
};

static DWORD WINAPI InputThread(LPVOID) {
    if (!DI_Init()) {
        Log("input: DirectInput unavailable, mod inactive");
        return 0;
    }
    HWND hwnd = CreateHiddenWindow();
    ParseAll();
    OpenDevices(hwnd);

    float digital = 0.0f;       // ramped clutch from buttons / keys
    int latched = -1;
    bool prevGearDown[kMaxGears + 1] = {};
    bool prevNeutral = false, prevToggle = false, prevBlow = false, prevRepair = false, prevMenu = false;
    Repeater nav[4];
    double last = Now(), lastScan = last, lastLog = 0;
    int logGear = -2;
    float logPedal = -1;
    timeBeginPeriod(1);

    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);

        double now = Now();
        float dt = (float)Clamp(now - last, 0.0, 0.1);
        last = now;

        bool missing = false;
        for (int j = 0; j < kMaxJoys; j++) {
            if (!s_joyWanted[j]) continue;
            if (s_joy[j].dev && !DI_Read(s_joy[j])) {
                Log("input: joy%d \"%s\" lost", j, s_joy[j].name.c_str());
                DI_Close(s_joy[j]);
            }
            if (!s_joy[j].dev) missing = true;
        }
        if (missing && now - lastScan > 3.0) {
            lastScan = now;
            OpenDevices(hwnd);
        }
        bool focus = GameHasFocus();

        // Clutch: deepest of all bound pedals / buttons.
        float pedal = 0.0f;
        bool digitalDown = false, clutchBound = false;
        for (size_t i = 0; i < s_clutch.size(); i++) {
            const Binding& b = s_clutch[i];
            if (!BindingUsable(b)) continue;
            clutchBound = true;
            if (b.kind != BK_AXIS) {
                digitalDown |= Down(b, focus);
                continue;
            }
            float raw = (float)s_joy[b.joy].state.axis[b.index];
            AxisCal& c = s_cal[i];
            if (!c.known) {
                // Uncalibrated: assume the pedal is up shortly after the device opens (some devices
                // report zeros at first) and that it rests at one end of its travel.
                if (now - s_joyOpened[b.joy] < 0.5) continue;
                c.known = true;
                c.released = raw > 32767.0f ? 65535.0f : 0.0f;
                c.pressed = 65535.0f - c.released;
                Log("input: clutch %s learned released=%d pressed=%d (run hshiftertool setup to calibrate)",
                    BindingName(b).c_str(), (int)c.released, (int)c.pressed);
            }
            float p = (raw - c.released) / (c.pressed - c.released);
            float dz = g_cfg.deadzone;
            p = Clamp((p - dz) / (1.0f - 2.0f * dz), 0.0f, 1.0f);
            pedal = std::max(pedal, p);
        }
        if (digitalDown)
            digital = g_cfg.buttonPressTime > 0 ? std::min(1.0f, digital + dt / g_cfg.buttonPressTime) : 1.0f;
        else
            digital = g_cfg.buttonReleaseTime > 0 ? std::max(0.0f, digital - dt / g_cfg.buttonReleaseTime) : 0.0f;
        pedal = std::max(pedal, digital);

        // Shifter.
        bool shifterBound = false;
        int stick = -1, count = 0;
        for (int g = 0; g <= kMaxGears; g++) {
            if (AnyUsable(s_gear[g])) shifterBound = true;
            bool down = AnyDown(s_gear[g], focus);
            if (down) {
                stick = g;
                count++;
            }
            if (down && !prevGearDown[g]) latched = g;
            prevGearDown[g] = down;
        }
        if (count > 1) stick = -1;      // two gates at once: treat as between gears
        bool neutralDown = AnyDown(s_neutral, focus);
        if (neutralDown && !prevNeutral) latched = -1;
        prevNeutral = neutralDown;
        if (g_cfg.latching) stick = latched;

        bool toggleDown = AnyDown(s_toggle, focus);
        bool blowDown = AnyDown(s_blow, focus);
        bool repairDown = AnyDown(s_repair, focus);
        bool menuDown = AnyDown(s_menu, focus);
        uint32_t navPresses[4];
        for (int i = 0; i < 4; i++) navPresses[i] = nav[i].Update(AnyDown(s_nav[i], focus), now);

        EnterCriticalSection(&s_lock);
        s_snap.clutchBound = clutchBound;
        s_snap.clutchPedal = pedal;
        s_snap.shifterBound = shifterBound;
        s_snap.stickGear = stick;
        if (toggleDown && !prevToggle) s_snap.toggles++;
        if (blowDown && !prevBlow) s_snap.blows++;
        if (repairDown && !prevRepair) s_snap.repairs++;
        if (menuDown && !prevMenu) s_snap.menu++;
        for (int i = 0; i < 4; i++) s_snap.menuNav[i] += navPresses[i];
        LeaveCriticalSection(&s_lock);
        prevToggle = toggleDown;
        prevBlow = blowDown;
        prevRepair = repairDown;
        prevMenu = menuDown;

        if (g_cfg.logInputs && now - lastLog > 0.1 && (stick != logGear || fabsf(pedal - logPedal) > 0.05f)) {
            lastLog = now;
            logGear = stick;
            logPedal = pedal;
            Log("input: stick=%s clutch=%.2f", stick < 0 ? "N" : stick == 0 ? "R" : std::to_string(stick).c_str(), pedal);
        }
        Sleep(2);
    }
}

void Input_Start() {
    if (InterlockedExchange(&s_started, 1)) return;
    InitializeCriticalSection(&s_lock);
    HANDLE t = CreateThread(nullptr, 0, InputThread, nullptr, 0, nullptr);
    if (t) {
        SetThreadPriority(t, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(t);
    }
}

InputSnapshot Input_Get() {
    InputSnapshot s;
    if (!s_started) return s;
    EnterCriticalSection(&s_lock);
    s = s_snap;
    LeaveCriticalSection(&s_lock);
    return s;
}
