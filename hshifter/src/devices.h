// DirectInput game-controller access and input bindings. Shared by the mod and hshiftertool.
#pragma once
#include "common.h"

// Our own data format: 8 axes, 4 POV hats, 128 buttons (same objects as DIJOYSTATE2's first part).
struct JoyState {
    LONG  axis[8];      // X Y Z RX RY RZ Slider0 Slider1, range 0..65535
    DWORD pov[4];       // hundredths of a degree, LOWORD 0xFFFF = centred
    BYTE  btn[128];
};
extern const char* const kAxisNames[8];   // "x" "y" "z" "rx" "ry" "rz" "s0" "s1"

struct JoyDevice {
    GUID guid{};
    std::string name;
    IDirectInputDevice8A* dev = nullptr;
    bool hasAxis[8] = {};
    int  numButtons = 0;
    int  numPovs = 0;
    JoyState state{};
    bool ok = false;                // last read succeeded
};

// Loads the system dinput8.dll (not a proxy in the game folder) and creates IDirectInput8A.
bool DI_Init();
// Attached game controllers, in DirectInput enumeration order.
std::vector<JoyDevice> DI_Enumerate();
bool DI_Open(JoyDevice& d, HWND hwnd);
bool DI_Read(JoyDevice& d);
void DI_Close(JoyDevice& d);

// ---------------------------------------------------------------------------
// Bindings: "key:LSHIFT", "joy0:b12", "joy1:s0", "joy0:-rz", "joy0:pov0:up"; lists are comma separated
// ---------------------------------------------------------------------------
enum BindKind { BK_KEY, BK_AXIS, BK_BUTTON, BK_POV };
struct Binding {
    BindKind kind = BK_KEY;
    int joy = 0;        // device slot (JoyN)
    int index = 0;      // axis / button / pov index, or virtual key for BK_KEY
    int povDir = 0;     // 0 up 1 right 2 down 3 left
    bool invert = false;// axis: "-" prefix, pressed end is the low end
};
bool ParseBindings(const std::string& text, std::vector<Binding>& out, std::string* err);
std::string BindingName(const Binding& b);
bool PovMatches(DWORD pov, int dir);
int  ParseKeyName(const char* s);
