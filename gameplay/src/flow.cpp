// Career flow: the first safehouse visit.
//
// After the prologue (D-Day) and the first car from the car lot, the career runs the gameplay
// script "drive_to_target": GPS to the safehouse, and on arrival (its trigger) at_target calls
// Game.DoSafeHouseIntro, which plays Mia's movie (storyfmv_saf25), then SafeHouseRivalBio.fng and
// the rival flow, which ends the career intro (CareerSettings flag CS_INTRO_DONE, bin 16 -> 15).
// The safehouse's own door is a separate script, menu_gate: it always runs
// MenuZoneTrigger::RequestDoAction, which quits to the plain safehouse menu (MainMenu_Sub.fng).
// If drive_to_target isn't running (it ends straight away when Game.SkipCareerIntro() is true),
// the door is all that's left: the safehouse menu, "Resume Career" back to the street, the intro
// never plays and the career can't go on.
//
// So the door starts the intro itself while the career is in exactly that state: career mode,
// still bin 16 (the prologue's bin), the D-Day races done (as CareerSettings::ResumeCareer checks)
// and no CS_INTRO_DONE. When the intro was just started by the script, the door's quit to the
// plain menu is dropped instead, so it can't replace the intro's.
#include "flow.h"
#include "common.h"
#include <cstring>
#include <initializer_list>

namespace {

constexpr uintptr_t kZoneRequestDoAction = 0x58C640;  // MenuZoneTrigger::RequestDoAction(), thiscall; +0x14 zone type
constexpr uintptr_t kDoSafeHouseIntro = 0x605560;     // Game_DoSafeHouseIntro(GRuntimeInstance* or null), cdecl
constexpr uintptr_t kFEDatabase = 0x91CF90;           // cFrontendDatabase*: +0x10 player 1's profile, +0x12C FEGameMode
constexpr int kFEGameMode = 0x12C;                    // bit 0 career
constexpr int kCareerFlags = 0xAC;                    // profile: CareerSettings (+0xA8) SpecialFlags
constexpr int kCareerBin = 0xB0;                      // profile: CareerSettings CurrentBin (16 = prologue)
constexpr uint32_t kIntroDone = 1u << 5;              // CS_INTRO_DONE
constexpr uintptr_t kRaceDatabase = 0x91E004;         // GRaceDatabase*
constexpr uintptr_t kStringHash32 = 0x4519D0;         // Attrib::StringHash32(const char*), cdecl
constexpr uintptr_t kGetRaceFromHash = 0x5FB710;      // GRaceDatabase::GetRaceFromHash(hash), thiscall
constexpr uintptr_t kGetEventHash = 0x5FAB40;         // GRaceParameters::GetEventHash(), thiscall
constexpr uintptr_t kCheckRaceScoreFlags = 0x5DCA70;  // GRaceDatabase::CheckRaceScoreFlags(hash, 2 = complete), thiscall
constexpr const char* kDDayEndRace = "16.2.1";        // GRaceDatabase::GetDDayEndRace
// Why the career script would skip the intro (Game_SkipCareerIntro, 0x612490), logged.
constexpr uintptr_t kSkipFE = 0x926064, kSkipCareerIntroCheat = 0x926125, kSkipDDayRaces = 0x926126;
constexpr uintptr_t kSkipNISs = 0x9260A4, kSkipMovies = 0x926144;

bool Expect(uintptr_t at, std::initializer_list<uint8_t> bytes) {
    const uint8_t* p = (const uint8_t*)at;
    size_t i = 0;
    for (uint8_t b : bytes)
        if (p[i++] != b) return false;
    return true;
}

// Jumps from `at` to `hook`; returns a trampoline running the `stolen` bytes and jumping back.
void* Detour(uintptr_t at, size_t stolen, void* hook) {
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, (void*)at, stolen);
    tramp[stolen] = 0xE9;
    *(int32_t*)(tramp + stolen + 1) = (int32_t)((at + stolen) - ((uintptr_t)tramp + stolen + 5));
    DWORD old;
    VirtualProtect((void*)at, stolen, PAGE_EXECUTE_READWRITE, &old);
    uint8_t* p = (uint8_t*)at;
    p[0] = 0xE9;
    *(int32_t*)(p + 1) = (int32_t)((uintptr_t)hook - (at + 5));
    for (size_t i = 5; i < stolen; i++) p[i] = 0x90;
    VirtualProtect((void*)at, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, stolen);
    return tramp;
}

void(__fastcall* s_requestDoAction)(void*, void*);
void(__cdecl* s_doSafeHouseIntro)(void*);
double s_introAt = -1;   // Now() of the last intro start

uint8_t* Profile() {
    uint8_t* db = *(uint8_t**)kFEDatabase;
    return db ? *(uint8_t**)(db + 0x10) : nullptr;
}

bool DDayDone() {
    if (*(uint8_t*)kSkipDDayRaces) return true;
    void* races = *(void**)kRaceDatabase;
    if (!races) return false;
    uint32_t hash = ((uint32_t(__cdecl*)(const char*))kStringHash32)(kDDayEndRace);
    void* race = ((void*(__fastcall*)(void*, void*, uint32_t))kGetRaceFromHash)(races, nullptr, hash);
    if (!race) return false;
    uint32_t ev = ((uint32_t(__fastcall*)(void*, void*))kGetEventHash)(race, nullptr);
    return ((bool(__fastcall*)(void*, void*, uint32_t, int))kCheckRaceScoreFlags)(races, nullptr, ev, 2);
}

void LogCareer(const char* when) {
    uint8_t* db = *(uint8_t**)kFEDatabase;
    uint8_t* p = Profile();
    Log("flow: %s: game mode 0x%X, bin %d, career flags 0x%X, D-Day %s; SkipFE %d, SkipCareerIntro %d, "
        "SkipDDayRaces %d, SkipNISs %d, SkipMovies %d",
        when, db ? *(uint32_t*)(db + kFEGameMode) : 0, p ? p[kCareerBin] : -1, p ? *(uint32_t*)(p + kCareerFlags) : 0,
        DDayDone() ? "done" : "not done", *(int*)kSkipFE, *(uint8_t*)kSkipCareerIntroCheat, *(uint8_t*)kSkipDDayRaces,
        *(int*)kSkipNISs, *(int*)kSkipMovies);
}

// Career between the first car and Mia's intro: what the safehouse door should start.
bool IntroPending() {
    uint8_t* db = *(uint8_t**)kFEDatabase;
    uint8_t* p = Profile();
    if (!db || !p || !(*(uint32_t*)(db + kFEGameMode) & 1)) return false;
    return p[kCareerBin] == 16 && !(*(uint32_t*)(p + kCareerFlags) & kIntroDone) && DDayDone();
}

void __cdecl Hook_DoSafeHouseIntro(void* instance) {
    s_introAt = Now();
    LogCareer("first safehouse visit (Mia's intro)");
    s_doSafeHouseIntro(instance);
}

void __fastcall Hook_RequestDoAction(uint8_t* zone, void* edx) {
    const char* type = *(const char**)(zone + 0x14);
    if (!type || IsBadStringPtrA(type, 32) || strcmp(type, "safehouse") != 0) {
        s_requestDoAction(zone, edx);
        return;
    }
    if (s_introAt >= 0 && Now() - s_introAt < 15.0) {
        Log("flow: safehouse door after the intro started: the plain safehouse menu skipped");
        return;
    }
    if (IntroPending()) {
        LogCareer("safehouse door with the career intro not played");
        Log("flow: starting Mia's intro (the career script didn't)");
        Hook_DoSafeHouseIntro(nullptr);
        return;
    }
    LogCareer("safehouse door");
    s_requestDoAction(zone, edx);
}

}  // namespace

void Flow_Install() {
    // push -1; push <handler> / mov eax, fs:[0]: both move as they are.
    if (!Expect(kZoneRequestDoAction, {0x6A, 0xFF, 0x68}) || !Expect(kDoSafeHouseIntro, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00})) {
        Log("flow: safehouse code not as expected, first-visit fix unavailable");
        return;
    }
    s_doSafeHouseIntro = (void(__cdecl*)(void*))Detour(kDoSafeHouseIntro, 6, (void*)&Hook_DoSafeHouseIntro);
    if (!s_doSafeHouseIntro) return;
    s_requestDoAction = (void(__fastcall*)(void*, void*))Detour(kZoneRequestDoAction, 7, (void*)&Hook_RequestDoAction);
    Log("flow: installed (first safehouse visit)");
}
