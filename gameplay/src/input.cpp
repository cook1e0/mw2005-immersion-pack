#include "input.h"
#include "devices.h"
#include <math.h>
#include <mmsystem.h>

static CRITICAL_SECTION s_lock;
static InputSnapshot s_snap;
static volatile LONG s_started;

static std::vector<Binding> s_clutch, s_gear[kMaxGears + 1], s_neutral, s_toggle, s_up, s_down, s_blow, s_repair, s_hit, s_pop, s_menu, s_recenter, s_nav[4];
static std::vector<Binding> s_steer, s_pedal[2], s_button[BTN_COUNT], s_seat[SEAT_COUNT];
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
    return CreateWindowExA(WS_EX_TOOLWINDOW, "STATIC", "nfsmw_gameplay", WS_POPUP, 0, 0, 1, 1,
                           nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
}

static bool GameHasFocus() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

// Binding capture, shared with the game thread under s_lock.
enum { CAP_IDLE, CAP_REQUESTED, CAP_ARMED, CAP_DONE };
static int s_capState = CAP_IDLE;
static std::string s_capResult;
static int s_capRest = -1;
static bool s_reparse = false;

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
    parse("ShiftUp", g_cfg.shiftUp, s_up);
    parse("ShiftDown", g_cfg.shiftDown, s_down);
    parse("DebugBlow", g_cfg.debugBlow, s_blow);
    parse("DebugRepair", g_cfg.debugRepair, s_repair);
    parse("DebugDamage", g_cfg.debugDamage, s_hit);
    parse("DebugPopTires", g_cfg.debugPopTires, s_pop);
    parse("Menu", g_cfg.menu, s_menu);
    parse("RecenterVr", g_cfg.recenterVr, s_recenter);
    parse("MenuUp", g_cfg.menuUp, s_nav[0]);
    parse("MenuDown", g_cfg.menuDown, s_nav[1]);
    parse("MenuLeft", g_cfg.menuLeft, s_nav[2]);
    parse("MenuRight", g_cfg.menuRight, s_nav[3]);
    parse("Steering", g_cfg.steer, s_steer);
    parse("Accelerate", g_cfg.throttle, s_pedal[0]);
    parse("Brake", g_cfg.brake, s_pedal[1]);
    for (int i = 0; i < BTN_COUNT; i++) parse(kButtonKeys[i], g_cfg.button[i], s_button[i]);
    for (int i = 0; i < SEAT_COUNT; i++) parse(kSeatKeys[i], g_cfg.seat[i], s_seat[i]);

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
static void OpenDevices(HWND hwnd, bool all = false) {
    std::vector<JoyDevice> attached = DI_Enumerate();
    if (all)
        for (size_t j = 0; j < attached.size() && j < (size_t)kMaxJoys; j++) s_joyWanted[j] = true;
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
        case BK_AXIS: {
            float travel = s_joy[b.joy].state.axis[b.index] - 32767.5f;
            return s_joy[b.joy].ok && (b.invert ? -travel : travel) > 24000.0f;
        }
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

// Steering, -1 left .. +1 right: the first usable axis binding, scaled so that SteeringLock
// degrees of wheel rotation (of WheelRotation in all) give full lock, then shaped by
// SteeringLinearity (output = input ^ linearity, so full lock stays at the steering lock).
static bool ReadSteer(float* out, float* wheelDeg) {
    for (auto& b : s_steer) {
        if (b.kind != BK_AXIS || !BindingUsable(b)) continue;
        float v = Clamp(((float)s_joy[b.joy].state.axis[b.index] - 32767.5f) / 32767.5f, -1.0f, 1.0f);
        if (b.invert) v = -v;
        *wheelDeg = v * g_cfg.wheelRotation * 0.5f;
        v *= g_cfg.wheelRotation / g_cfg.steeringLock * g_cfg.steerSensitivity;
        float dz = g_cfg.steerDeadzone, a = fabsf(v);
        a = a <= dz ? 0.0f : (a - dz) / (1.0f - dz);
        a = powf(fminf(a, 1.0f), g_cfg.steerLinearity);
        *out = Clamp(v < 0 ? -a : a, -1.0f, 1.0f);
        return true;
    }
    return false;
}

// Pedal travel 0..1, deepest of the bindings (keys and buttons are 0 or 1). `rest` is the raw axis
// value with the pedal up (-1: the end opposite to the direction it was bound in).
static bool ReadPedal(const std::vector<Binding>& list, int rest, bool focus, float* out) {
    bool bound = false;
    float p = 0.0f;
    for (auto& b : list) {
        if (!BindingUsable(b)) continue;
        bound = true;
        if (b.kind != BK_AXIS) {
            if (Down(b, focus)) p = 1.0f;
            continue;
        }
        float raw = (float)s_joy[b.joy].state.axis[b.index];
        float pressed = b.invert ? 0.0f : 65535.0f;
        float released = rest >= 0 ? (float)rest : 65535.0f - pressed;
        if (fabsf(pressed - released) < 1000.0f) continue;
        float v = (raw - released) / (pressed - released);
        float dz = g_cfg.pedalDeadzone;
        p = std::max(p, Clamp((v - dz) / (1.0f - 2.0f * dz), 0.0f, 1.0f));
    }
    *out = p;
    return bound;
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

// Capture: remembers what is held when armed, then takes the first new press or big axis move.
struct Capture {
    bool keyHeld[256];
    JoyState base[kMaxJoys];
    bool baseOk[kMaxJoys];
    double armAt;

    void Arm(double now) {
        for (int vk = 0; vk < 256; vk++) keyHeld[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
        for (int j = 0; j < kMaxJoys; j++) {
            baseOk[j] = s_joy[j].dev && s_joy[j].ok;
            if (baseOk[j]) base[j] = s_joy[j].state;
        }
        armAt = now;
    }

    // Returns true with the binding text when something new was pressed.
    bool Poll(bool focus, std::string& out, int& rest) {
        rest = -1;
        for (int vk = 8; vk < 256; vk++) {
            if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;   // the L/R codes are reported too
            bool down = focus && (GetAsyncKeyState(vk) & 0x8000);
            if (!down) keyHeld[vk] = false;
            else if (!keyHeld[vk]) {
                Binding b;
                b.kind = BK_KEY;
                b.index = vk;
                out = BindingName(b);
                return true;
            }
        }
        for (int j = 0; j < kMaxJoys; j++) {
            const JoyDevice& d = s_joy[j];
            if (!d.dev || !d.ok) continue;
            if (!baseOk[j]) {
                base[j] = d.state;
                baseOk[j] = true;
                continue;
            }
            JoyState& b0 = base[j];
            Binding b;
            b.joy = j;
            for (int i = 0; i < 128; i++) {
                bool down = (d.state.btn[i] & 0x80) != 0;
                if (!down) b0.btn[i] = 0;
                else if (!(b0.btn[i] & 0x80)) {
                    b.kind = BK_BUTTON;
                    b.index = i;
                    out = BindingName(b);
                    return true;
                }
            }
            for (int p = 0; p < 4; p++) {
                if (LOWORD(d.state.pov[p]) == 0xFFFF) {
                    b0.pov[p] = d.state.pov[p];
                    continue;
                }
                for (int dir = 0; dir < 4; dir++)
                    if (PovMatches(d.state.pov[p], dir) && !PovMatches(b0.pov[p], dir)) {
                        b.kind = BK_POV;
                        b.index = p;
                        b.povDir = dir;
                        out = BindingName(b);
                        return true;
                    }
            }
            for (int a = 0; a < 8; a++) {
                if (!d.hasAxis[a]) continue;
                LONG delta = d.state.axis[a] - b0.axis[a];
                if (labs(delta) < 20000) continue;
                b.kind = BK_AXIS;
                b.index = a;
                b.invert = delta < 0;
                out = BindingName(b);
                rest = (int)b0.axis[a];
                return true;
            }
        }
        return false;
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
    bool prevNeutral = false, prevToggle = false, prevUp = false, prevDown = false, prevBlow = false, prevRepair = false, prevHit = false, prevPop = false, prevMenu = false, prevRecenter = false;
    Repeater nav[4];
    double last = Now(), lastScan = last, lastLog = 0;
    int logGear = -2;
    float logPedal = -1;
    static Capture cap;
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

        EnterCriticalSection(&s_lock);
        if (s_reparse) {
            s_reparse = false;
            ParseAll();
            lastScan = 0;   // open newly wanted devices below
        }
        int capState = s_capState;
        LeaveCriticalSection(&s_lock);
        if (capState == CAP_REQUESTED) {
            OpenDevices(hwnd, true);
            cap.Arm(now);
            EnterCriticalSection(&s_lock);
            if (s_capState == CAP_REQUESTED) s_capState = CAP_ARMED;
            LeaveCriticalSection(&s_lock);
        } else if (capState == CAP_ARMED && now - cap.armAt > 0.25) {
            std::string text;
            int rest;
            if (cap.Poll(focus, text, rest)) {
                EnterCriticalSection(&s_lock);
                if (s_capState == CAP_ARMED) {
                    s_capState = CAP_DONE;
                    s_capResult = text;
                    s_capRest = rest;
                }
                LeaveCriticalSection(&s_lock);
                Log("input: captured %s", text.c_str());
            }
        }

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
                Log("input: clutch %s learned released=%d pressed=%d (bind the pedal in the F4 menu to calibrate)",
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
        bool upDown = AnyDown(s_up, focus), downDown = AnyDown(s_down, focus);
        bool blowDown = AnyDown(s_blow, focus);
        bool repairDown = AnyDown(s_repair, focus);
        bool hitDown = AnyDown(s_hit, focus);
        bool popDown = AnyDown(s_pop, focus);
        bool menuDown = AnyDown(s_menu, focus);
        bool recenterDown = AnyDown(s_recenter, focus);
        uint32_t navPresses[4];
        for (int i = 0; i < 4; i++) navPresses[i] = nav[i].Update(AnyDown(s_nav[i], focus), now);

        // Driving controls.
        float steer = 0.0f, wheelDeg = 0.0f, pedals[2] = {};
        bool steerBound = ReadSteer(&steer, &wheelDeg);
        bool pedalBound[2];
        pedalBound[0] = ReadPedal(s_pedal[0], g_cfg.throttleRest, focus, &pedals[0]);
        pedalBound[1] = ReadPedal(s_pedal[1], g_cfg.brakeRest, focus, &pedals[1]);
        uint32_t buttonBound = 0, buttonDown = 0, seatDown = 0;
        for (int i = 0; i < BTN_COUNT; i++) {
            if (AnyUsable(s_button[i])) buttonBound |= 1u << i;
            if (AnyDown(s_button[i], focus)) buttonDown |= 1u << i;
        }
        for (int i = 0; i < SEAT_COUNT; i++)
            if (AnyDown(s_seat[i], focus)) seatDown |= 1u << i;

        EnterCriticalSection(&s_lock);
        s_snap.steerBound = steerBound;
        s_snap.steer = steer;
        s_snap.wheelDeg = wheelDeg;
        for (int i = 0; i < 2; i++) {
            s_snap.pedalBound[i] = pedalBound[i];
            s_snap.pedal[i] = pedals[i];
        }
        s_snap.buttonBound = buttonBound;
        s_snap.buttonDown = buttonDown;
        s_snap.seatDown = seatDown;
        s_snap.clutchBound = clutchBound;
        s_snap.clutchPedal = pedal;
        s_snap.shifterBound = shifterBound;
        s_snap.stickGear = stick;
        if (toggleDown && !prevToggle) s_snap.toggles++;
        if (upDown && !prevUp) s_snap.shiftUp++;
        if (downDown && !prevDown) s_snap.shiftDown++;
        if (blowDown && !prevBlow) s_snap.blows++;
        if (repairDown && !prevRepair) s_snap.repairs++;
        if (hitDown && !prevHit) s_snap.hits++;
        if (popDown && !prevPop) s_snap.pops++;
        if (menuDown && !prevMenu) s_snap.menu++;
        if (recenterDown && !prevRecenter) s_snap.recenters++;
        for (int i = 0; i < 4; i++) s_snap.menuNav[i] += navPresses[i];
        LeaveCriticalSection(&s_lock);
        prevToggle = toggleDown;
        prevUp = upDown;
        prevDown = downDown;
        prevBlow = blowDown;
        prevRepair = repairDown;
        prevHit = hitDown;
        prevPop = popDown;
        prevMenu = menuDown;
        prevRecenter = recenterDown;

        if (g_cfg.logInputs && now - lastLog > 0.1 && (stick != logGear || fabsf(pedal - logPedal) > 0.05f)) {
            lastLog = now;
            logGear = stick;
            logPedal = pedal;
            Log("input: stick=%s clutch=%.2f", stick < 0 ? "N" : stick == 0 ? "R" : std::to_string(stick).c_str(), pedal);
        }
        Sleep(2);
    }
}

void Input_BeginCapture() {
    Input_Start();
    EnterCriticalSection(&s_lock);
    s_capState = CAP_REQUESTED;
    s_capResult.clear();
    LeaveCriticalSection(&s_lock);
}

void Input_CancelCapture() {
    if (!s_started) return;
    EnterCriticalSection(&s_lock);
    s_capState = CAP_IDLE;
    LeaveCriticalSection(&s_lock);
}

bool Input_Captured(std::string* binding, int* axisRest) {
    if (!s_started) return false;
    EnterCriticalSection(&s_lock);
    bool done = s_capState == CAP_DONE;
    if (done) {
        *binding = s_capResult;
        *axisRest = s_capRest;
        s_capState = CAP_IDLE;
    }
    LeaveCriticalSection(&s_lock);
    return done;
}

void Input_SetBinding(std::string& field, const std::string& value) {
    Input_Start();
    EnterCriticalSection(&s_lock);
    field = value;
    s_reparse = true;
    LeaveCriticalSection(&s_lock);
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
