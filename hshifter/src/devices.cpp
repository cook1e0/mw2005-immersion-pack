#include "devices.h"
#include <stddef.h>
#include <stdio.h>
#include <ctype.h>

const char* const kAxisNames[8] = {"x", "y", "z", "rx", "ry", "rz", "s0", "s1"};

static IDirectInput8A* s_di;

static DIOBJECTDATAFORMAT s_objs[8 + 4 + 128];
static DIDATAFORMAT s_fmt;

static void BuildFormat() {
    static const GUID* axisGuid[8] = {&GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis,
                                      &GUID_RyAxis, &GUID_RzAxis, &GUID_Slider, &GUID_Slider};
    int n = 0;
    for (int i = 0; i < 8; i++)
        s_objs[n++] = {axisGuid[i], (DWORD)(offsetof(JoyState, axis) + i * sizeof(LONG)),
                       DIDFT_AXIS | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0};
    for (int i = 0; i < 4; i++)
        s_objs[n++] = {&GUID_POV, (DWORD)(offsetof(JoyState, pov) + i * sizeof(DWORD)),
                       DIDFT_POV | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0};
    for (int i = 0; i < 128; i++)
        s_objs[n++] = {nullptr, (DWORD)(offsetof(JoyState, btn) + i),
                       DIDFT_BUTTON | DIDFT_ANYINSTANCE | DIDFT_OPTIONAL, 0};
    s_fmt = {sizeof(DIDATAFORMAT), sizeof(DIOBJECTDATAFORMAT), DIDF_ABSAXIS, sizeof(JoyState), (DWORD)n, s_objs};
}

bool DI_Init() {
    if (s_di) return true;
    BuildFormat();
    // The game folder may hold a dinput8.dll proxy (e.g. a force feedback mod) that forces
    // devices into exclusive mode; load the real one by full path.
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat(path, "\\dinput8.dll");
    HMODULE mod = LoadLibraryA(path);
    if (!mod) {
        Log("input: cannot load %s (%lu)", path, GetLastError());
        return false;
    }
    typedef HRESULT(WINAPI * Create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    Create_t create = (Create_t)GetProcAddress(mod, "DirectInput8Create");
    if (!create) return false;
    HRESULT hr = create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, (void**)&s_di, nullptr);
    if (FAILED(hr)) {
        Log("input: DirectInput8Create failed 0x%08lx", hr);
        s_di = nullptr;
        return false;
    }
    return true;
}

static BOOL CALLBACK EnumCb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx) {
    auto* list = (std::vector<JoyDevice>*)ctx;
    JoyDevice d;
    d.guid = inst->guidInstance;
    d.name = inst->tszInstanceName;
    list->push_back(d);
    return DIENUM_CONTINUE;
}

std::vector<JoyDevice> DI_Enumerate() {
    std::vector<JoyDevice> list;
    if (s_di) s_di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCb, &list, DIEDFL_ATTACHEDONLY);
    return list;
}

static BOOL CALLBACK ObjCb(LPCDIDEVICEOBJECTINSTANCEA obj, LPVOID ctx) {
    JoyDevice* d = (JoyDevice*)ctx;
    const GUID& g = obj->guidType;
    if (g == GUID_XAxis) d->hasAxis[0] = true;
    else if (g == GUID_YAxis) d->hasAxis[1] = true;
    else if (g == GUID_ZAxis) d->hasAxis[2] = true;
    else if (g == GUID_RxAxis) d->hasAxis[3] = true;
    else if (g == GUID_RyAxis) d->hasAxis[4] = true;
    else if (g == GUID_RzAxis) d->hasAxis[5] = true;
    else if (g == GUID_Slider) d->hasAxis[d->hasAxis[6] ? 7 : 6] = true;
    return DIENUM_CONTINUE;
}

bool DI_Open(JoyDevice& d, HWND hwnd) {
    if (!s_di) return false;
    if (FAILED(s_di->CreateDevice(d.guid, &d.dev, nullptr))) return false;
    if (FAILED(d.dev->SetDataFormat(&s_fmt))) {
        DI_Close(d);
        return false;
    }
    // Non-exclusive: works next to the game's (or an FFB mod's) exclusive handle on the same wheel.
    d.dev->SetCooperativeLevel(hwnd, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
    DIPROPRANGE range = {};
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    range.diph.dwHow = DIPH_DEVICE;
    range.lMin = 0;
    range.lMax = 65535;
    d.dev->SetProperty(DIPROP_RANGE, &range.diph);
    DIPROPDWORD dz = {};
    dz.diph.dwSize = sizeof(dz);
    dz.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    dz.diph.dwHow = DIPH_DEVICE;
    dz.dwData = 0;
    d.dev->SetProperty(DIPROP_DEADZONE, &dz.diph);
    DIDEVCAPS caps = {};
    caps.dwSize = sizeof(caps);
    if (SUCCEEDED(d.dev->GetCapabilities(&caps))) {
        d.numButtons = (int)caps.dwButtons;
        d.numPovs = (int)caps.dwPOVs;
    }
    memset(d.hasAxis, 0, sizeof(d.hasAxis));
    d.dev->EnumObjects(ObjCb, &d, DIDFT_AXIS);
    d.dev->Acquire();
    return true;
}

bool DI_Read(JoyDevice& d) {
    if (!d.dev) return d.ok = false;
    d.dev->Poll();
    HRESULT hr = d.dev->GetDeviceState(sizeof(JoyState), &d.state);
    if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
        d.dev->Acquire();
        d.dev->Poll();
        hr = d.dev->GetDeviceState(sizeof(JoyState), &d.state);
    }
    return d.ok = SUCCEEDED(hr);
}

void DI_Close(JoyDevice& d) {
    if (d.dev) {
        d.dev->Unacquire();
        d.dev->Release();
        d.dev = nullptr;
    }
    d.ok = false;
}

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
static const struct { const char* name; int vk; } kKeys[] = {
    {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"SHIFT", VK_SHIFT},
    {"LCTRL", VK_LCONTROL}, {"RCTRL", VK_RCONTROL}, {"CTRL", VK_CONTROL},
    {"LALT", VK_LMENU}, {"RALT", VK_RMENU}, {"ALT", VK_MENU},
    {"SPACE", VK_SPACE}, {"TAB", VK_TAB}, {"ENTER", VK_RETURN}, {"BACKSPACE", VK_BACK},
    {"CAPSLOCK", VK_CAPITAL}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"HOME", VK_HOME},
    {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
    {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT},
    {"NUMPAD0", VK_NUMPAD0}, {"NUMPAD1", VK_NUMPAD1}, {"NUMPAD2", VK_NUMPAD2}, {"NUMPAD3", VK_NUMPAD3},
    {"NUMPAD4", VK_NUMPAD4}, {"NUMPAD5", VK_NUMPAD5}, {"NUMPAD6", VK_NUMPAD6}, {"NUMPAD7", VK_NUMPAD7},
    {"NUMPAD8", VK_NUMPAD8}, {"NUMPAD9", VK_NUMPAD9}, {"MULTIPLY", VK_MULTIPLY}, {"ADD", VK_ADD},
    {"SUBTRACT", VK_SUBTRACT}, {"DECIMAL", VK_DECIMAL}, {"DIVIDE", VK_DIVIDE},
    {"MINUS", VK_OEM_MINUS}, {"PLUS", VK_OEM_PLUS}, {"COMMA", VK_OEM_COMMA}, {"PERIOD", VK_OEM_PERIOD},
    {"SEMICOLON", VK_OEM_1}, {"SLASH", VK_OEM_2}, {"GRAVE", VK_OEM_3}, {"LBRACKET", VK_OEM_4},
    {"BACKSLASH", VK_OEM_5}, {"RBRACKET", VK_OEM_6}, {"APOSTROPHE", VK_OEM_7},
};

int ParseKeyName(const char* s) {
    if (!s[0]) return 0;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return (int)strtol(s, nullptr, 16);
    if (!s[1] && (isalnum((unsigned char)s[0]))) return toupper((unsigned char)s[0]);
    if ((s[0] == 'F' || s[0] == 'f') && isdigit((unsigned char)s[1])) {
        int n = atoi(s + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    for (auto& k : kKeys)
        if (_stricmp(k.name, s) == 0) return k.vk;
    return 0;
}

static std::string KeyName(int vk) {
    for (auto& k : kKeys)
        if (k.vk == vk) return k.name;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    char buf[16];
    snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

static bool ParseOne(std::string t, Binding& b, std::string* err) {
    for (auto& ch : t) ch = (char)tolower((unsigned char)ch);
    auto fail = [&](const char* why) {
        if (err) *err = "'" + t + "': " + why;
        return false;
    };
    if (t.compare(0, 4, "key:") == 0) {
        b.kind = BK_KEY;
        b.index = ParseKeyName(t.c_str() + 4);
        return b.index ? true : fail("unknown key name");
    }
    if (t.compare(0, 3, "joy") != 0) return fail("expected key:<name> or joy<N>:<input>");
    const char* p = t.c_str() + 3;
    if (!isdigit((unsigned char)*p)) return fail("missing device number after 'joy'");
    b.joy = atoi(p);
    if (b.joy < 0 || b.joy >= kMaxJoys) return fail("device number out of range (0-7)");
    while (isdigit((unsigned char)*p)) p++;
    if (*p++ != ':') return fail("missing ':' after device");
    b.invert = false;
    if (*p == '-') {
        b.invert = true;
        p++;
    }
    if (p[0] == 'b' && isdigit((unsigned char)p[1])) {
        b.kind = BK_BUTTON;
        b.index = atoi(p + 1);
        return b.index < 128 ? true : fail("button index out of range (0-127)");
    }
    if (strncmp(p, "pov", 3) == 0 && isdigit((unsigned char)p[3])) {
        b.kind = BK_POV;
        b.index = p[3] - '0';
        if (b.index > 3) return fail("pov index out of range (0-3)");
        const char* dir = strchr(p, ':');
        if (!dir) return fail("pov needs a direction, e.g. pov0:up");
        dir++;
        static const char* dirs[4] = {"up", "right", "down", "left"};
        for (int i = 0; i < 4; i++)
            if (strcmp(dir, dirs[i]) == 0) {
                b.povDir = i;
                return true;
            }
        return fail("pov direction must be up/right/down/left");
    }
    for (int i = 0; i < 8; i++)
        if (strcmp(p, kAxisNames[i]) == 0) {
            b.kind = BK_AXIS;
            b.index = i;
            return true;
        }
    return fail("unknown input (use bN, x/y/z/rx/ry/rz/s0/s1, or povN:dir)");
}

bool ParseBindings(const std::string& text, std::vector<Binding>& out, std::string* err) {
    out.clear();
    size_t i = 0;
    bool ok = true;
    while (i <= text.size()) {
        size_t j = text.find(',', i);
        if (j == std::string::npos) j = text.size();
        std::string tok = text.substr(i, j - i);
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t") + 1);
        if (!tok.empty()) {
            Binding b;
            if (ParseOne(tok, b, err)) out.push_back(b);
            else ok = false;
        }
        i = j + 1;
    }
    return ok;
}

std::string BindingName(const Binding& b) {
    char buf[64];
    switch (b.kind) {
        case BK_KEY: return "key:" + KeyName(b.index);
        case BK_AXIS: snprintf(buf, sizeof(buf), "joy%d:%s%s", b.joy, b.invert ? "-" : "", kAxisNames[b.index]); break;
        case BK_BUTTON: snprintf(buf, sizeof(buf), "joy%d:b%d", b.joy, b.index); break;
        case BK_POV: {
            static const char* dirs[4] = {"up", "right", "down", "left"};
            snprintf(buf, sizeof(buf), "joy%d:pov%d:%s", b.joy, b.index, dirs[b.povDir]);
            break;
        }
    }
    return buf;
}

bool PovMatches(DWORD pov, int dir) {
    if (LOWORD(pov) == 0xFFFF) return false;
    int a = (int)(pov % 36000);
    int centre = dir * 9000;
    int d = abs(a - centre);
    if (d > 18000) d = 36000 - d;
    return d <= 4500;   // diagonals count for both neighbours
}
