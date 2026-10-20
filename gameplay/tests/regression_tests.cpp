// Exercise production input, settings migration and smoke selection without the game.
#include "../src/devices.cpp"
#include "../src/input.cpp"
#include "../src/vfx.cpp"
#include "../src/ini_migration.h"
#include "../src/controls.h"
#include <assert.h>
Config g_cfg;
volatile unsigned char gameAddressPadding[0x600000];
void Log(const char*, ...) {}
const char* GameDir() { return ""; }
double Now() { return 1; }
HudStatus Game_Hud() { return {}; }
const char* const kButtonKeys[6] = {"Handbrake", "Nitrous", "Speedbreaker", "ResetCar", "Camera", "LookBack"};

static bool player;
static bool __attribute__((thiscall)) IsPlayer(void*) { return player; }
static void TestBindings() {
    s_joy[0].dev = (IDirectInputDevice8A*)1;
    s_joy[0].ok = true;
    std::vector<Binding> bindings;
    for (bool inverse : {false, true}) {
        assert(ParseBindings(inverse ? "joy0:-y" : "joy0:y", bindings, nullptr));
        for (LONG raw : {0L, 32767L, 65535L}) {
            s_joy[0].state.axis[1] = raw;
            assert(Down(bindings[0], true) == (inverse ? raw == 0 : raw == 65535));
        }
        s_joy[0].ok = false;
        assert(!Down(bindings[0], true));
        s_joy[0].ok = true;
    }
    assert(ParseBindings("joy0:b3", bindings, nullptr));
    s_joy[0].state.btn[3] = 0x80;
    assert(Down(bindings[0], true));
    s_joy[0].state.btn[3] = 0;
    assert(!Down(bindings[0], true));
}

static void TestSteeringWheelAngle() {
    s_joy[0].dev = (IDirectInputDevice8A*)1;
    s_joy[0].ok = true;
    assert(ParseBindings("joy0:x", s_steer, nullptr));
    g_cfg.wheelRotation = 900.0f;
    g_cfg.steeringLock = 540.0f;
    g_cfg.steerSensitivity = 2.0f;
    g_cfg.steerDeadzone = 0.0f;
    g_cfg.steerLinearity = 1.0f;

    float steer, wheelDeg;
    s_joy[0].state.axis[0] = 0;
    assert(ReadSteer(&steer, &wheelDeg) && fabsf(wheelDeg + 450.0f) < 0.001f);
    s_joy[0].state.axis[0] = 32767;
    assert(ReadSteer(&steer, &wheelDeg) && fabsf(wheelDeg) < 0.01f);
    s_joy[0].state.axis[0] = 65535;
    assert(ReadSteer(&steer, &wheelDeg) && fabsf(wheelDeg - 450.0f) < 0.001f);
    s_joy[0].state.axis[0] = 70000;
    assert(ReadSteer(&steer, &wheelDeg) && fabsf(wheelDeg - 450.0f) < 0.001f);

    g_cfg.wheelRotation = 1080.0f;
    s_joy[0].state.axis[0] = 65535;
    assert(ReadSteer(&steer, &wheelDeg) && fabsf(wheelDeg - 540.0f) < 0.001f);
    s_steer.clear();
    s_joy[0].ok = false;
    s_joy[0].dev = nullptr;
}

static void TestSmoke() {
    assert((uintptr_t)gameAddressPadding < kIsViewAnchor &&
           (uintptr_t)gameAddressPadding + sizeof(gameAddressPadding) > kIsViewAnchor + 5);
    DWORD old;
    assert(VirtualProtect((void*)kIsViewAnchor, 5, PAGE_EXECUTE_READWRITE, &old));
    *(uint8_t*)kIsViewAnchor = 0xE9;
    *(int32_t*)(kIsViewAnchor + 1) = (int32_t)((uintptr_t)&IsPlayer - kIsViewAnchor - 5);
    FlushInstructionCache(GetCurrentProcess(), (void*)kIsViewAnchor, 5);
    unsigned char packet[0x94] = {};
    *(float*)(packet + kPkt_Health) = 0.1f;
    player = true;
    s_copKey = 77; // Avoid accessing the game's attribute database.
    assert(Vfx_DamageKey(nullptr, packet, 1234, 0) == 1234 && s_playerSmoke && s_smokeHealth == 0);
    player = false;
    *(float*)(packet + kPkt_Health) = 0.4f;
    assert(Vfx_DamageKey(nullptr, packet, 2345, 0) == 2345 && !s_playerSmoke && fabsf(s_smokeHealth - 0.4f) < 0.000001f);
    player = true;
    g_cfg.damageSmoke = false;
    s_playerSmoke = true;
    assert(Vfx_DamageKey(nullptr, packet, 1234, 0) == 1234 && !s_playerSmoke);
    g_cfg.damageSmoke = true;
    *(float*)(packet + kPkt_Health) = 1;
    assert(Vfx_DamageKey(nullptr, packet, 1234, 0) == 0 && !s_playerSmoke);
}

static void TestMigration() {
    char defaults[MAX_PATH], root[MAX_PATH];
    GetFullPathNameA("nfsmw_gameplay.ini", MAX_PATH, defaults, nullptr);
    uint64_t fingerprint = IniFingerprint(defaults);
    if (fingerprint != kGameplayDefaultIniFingerprint)
        fprintf(stderr, "Update kGameplayDefaultIniFingerprint for shipped defaults: 0x%llx\n", (unsigned long long)fingerprint);
    assert(fingerprint == kGameplayDefaultIniFingerprint);
    char relative[128];
    snprintf(relative, sizeof(relative), "build\\regression-settings-%lu-%lu", GetCurrentProcessId(), GetTickCount());
    GetFullPathNameA(relative, MAX_PATH, root, nullptr);
    assert(CreateDirectoryA(root, nullptr));
    std::string oldIni = std::string(root) + "\\nfsmw_hshifter.ini";
    std::string current = std::string(root) + "\\nfsmw_gameplay.ini";
    std::string backup = current + ".pre-hshifter.bak";
    auto legacy = [&] {
        assert(WritePrivateProfileStringA("Clutch", "Clutch", "joy0:-y", oldIni.c_str()));
        assert(WritePrivateProfileStringA("Shifter", "Gear1", "joy0:b3", oldIni.c_str()));
    };
    auto exists = [](const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
    auto clear = [&] {
        for (const auto& p : {oldIni, current, backup}) {
            WritePrivateProfileStringA(nullptr, nullptr, nullptr, p.c_str());
            if (exists(p)) assert(DeleteFileA(p.c_str()));
        }
    };
    legacy();
    assert(MigrateLegacyIni(oldIni.c_str(), current.c_str()));
    assert(!exists(oldIni) && exists(current) && !exists(backup));
    char value[64];
    GetPrivateProfileStringA("Clutch", "Clutch", "", value, sizeof(value), current.c_str());
    assert(strcmp(value, "joy0:-y") == 0);
    clear(); legacy();
    assert(CopyFileA(defaults, current.c_str(), TRUE));
    assert(MigrateLegacyIni(oldIni.c_str(), current.c_str()) && exists(backup) && !exists(oldIni));
    assert(IniFingerprint(backup.c_str()) == kGameplayDefaultIniFingerprint);
    GetPrivateProfileStringA("Clutch", "Clutch", "", value, sizeof(value), current.c_str());
    assert(strcmp(value, "joy0:-y") == 0);
    clear(); legacy();
    assert(CopyFileA(defaults, current.c_str(), TRUE));
    assert(WritePrivateProfileStringA("Clutch", "Strength", "1.1", current.c_str()));
    uint64_t customised = IniFingerprint(current.c_str());
    assert(!MigrateLegacyIni(oldIni.c_str(), current.c_str()));
    assert(exists(oldIni) && !exists(backup) && IniFingerprint(current.c_str()) == customised);
    clear(); legacy();
    assert(CopyFileA(defaults, current.c_str(), TRUE));
    assert(WritePrivateProfileStringA("Car.mycar", "Grip", "1.2", current.c_str()));
    assert(!MigrateLegacyIni(oldIni.c_str(), current.c_str()) && exists(oldIni));
    clear(); legacy();
    assert(CopyFileA(defaults, current.c_str(), TRUE));
    assert(CopyFileA(defaults, backup.c_str(), TRUE));
    assert(!MigrateLegacyIni(oldIni.c_str(), current.c_str()) && exists(oldIni));
    assert(IniFingerprint(current.c_str()) == kGameplayDefaultIniFingerprint);
    clear();
    assert(RemoveDirectoryA(root));
}
// Speed-sensitive wheel steering: full lock parked, falling smoothly and steadily with speed,
// close to the game's own values at the default, never below 1 - sensitivity, none when off.
static void TestSteeringLock() {
    assert(SteeringLockShare(0, 0.85f) == 1.0f);
    float prev = 1.0f;
    for (float kmh = 1; kmh < 400; kmh += 1) {
        float s = SteeringLockShare(kmh, 0.85f);
        assert(s < prev && s > 0.15f);
        assert(prev - s < 0.02f);                       // no kinks or jumps
        prev = s;
    }
    float at64 = 45 * SteeringLockShare(64, 0.85f), at130 = 45 * SteeringLockShare(130, 0.85f);
    assert(at64 > 16 && at64 < 23 && at130 > 10 && at130 < 15);
    assert(SteeringLockShare(200, 0.0f) == 1.0f);
    assert(SteeringLockShare(200, 0.5f) > SteeringLockShare(200, 0.85f));
}

int main() {
    TestSteeringLock();
    static_assert(sizeof(void*) == 4, "test the game layout");
    TestBindings(); TestSteeringWheelAngle(); TestSmoke(); TestMigration();
    puts("gameplay regression tests passed");
}
