// Career flow: the first safehouse visit (and, further down, the car handed back at a race start).
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
// The usual reason is an "intro done" flag the career can't have at bin 16 (a prepared profile):
// it is cleared when the script asks, so drive_to_target runs and the safehouse's trigger starts
// the intro on arrival. SkipMovies left on by the widescreen fix is turned off then too.
// As a fallback the door starts the intro itself while the career is in exactly that state: career mode,
// still bin 16 (the prologue's bin), the D-Day races done (as CareerSettings::ResumeCareer checks)
// and no CS_INTRO_DONE. When the intro was just started by the script, the door's quit to the
// plain menu is dropped instead, so it can't replace the intro's.
#include "flow.h"
#include "common.h"
#include "game.h"
#include "input.h"
#include "physics.h"
#include <math.h>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <set>
#include <cstdlib>

namespace {

constexpr uintptr_t kZoneRequestDoAction = 0x58C640;  // MenuZoneTrigger::RequestDoAction(), thiscall; +0x14 zone type
constexpr uintptr_t kDoSafeHouseIntro = 0x605560;     // Game_DoSafeHouseIntro(GRuntimeInstance* or null), cdecl
constexpr uintptr_t kSkipCareerIntro = 0x612490;      // bool Game_SkipCareerIntro() (Lua Game.SkipCareerIntro), cdecl
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
bool(__cdecl* s_skipCareerIntro)();
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

// Story movies (Mia, the Blacklist bios) end the moment they start while SkipMovies is set: the
// widescreen fix's SkipIntro leaves it on after the startup movies. Turned off once in the world.
void PlayMovies() {
    if (*(int*)kSkipMovies) {
        Log("flow: SkipMovies was on in game (startup movie skip left on), story movies play again");
        *(int*)kSkipMovies = 0;
    }
}

// CS_INTRO_DONE is only ever set by the rival flow after Mia's intro, together with bin 15 (or by
// the debug skip, also bin 15). With it set while the career is still at bin 16 (seen in a
// prepared profile, 0x1DF7F3), Game.SkipCareerIntro() says yes and drive_to_target ends at once:
// no GPS, no trigger at the safehouse, no intro. Such a flag is cleared before the script asks.
void RepairIntroFlag() {
    uint8_t* db = *(uint8_t**)kFEDatabase;
    uint8_t* p = Profile();
    if (!db || !p || !(*(uint32_t*)(db + kFEGameMode) & 1) || p[kCareerBin] != 16) return;
    uint32_t& flags = *(uint32_t*)(p + kCareerFlags);
    if (!(flags & kIntroDone)) return;
    Log("flow: career at bin 16 marked intro done (flags 0x%X): impossible in the game, mark cleared", flags);
    flags &= ~kIntroDone;
}

bool __cdecl Hook_SkipCareerIntro() {
    RepairIntroFlag();
    PlayMovies();
    bool skip = s_skipCareerIntro();
    static int logged;
    if (logged < 10) {
        logged++;
        LogCareer(skip ? "career script: skip the intro" : "career script: intro to come");
    }
    return skip;
}

void __cdecl Hook_DoSafeHouseIntro(void* instance) {
    PlayMovies();
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
    RepairIntroFlag();
    if (IntroPending()) {
        LogCareer("safehouse door with the career intro not played");
        Log("flow: starting Mia's intro (the career script didn't)");
        Hook_DoSafeHouseIntro(nullptr);
        return;
    }
    LogCareer("safehouse door");
    s_requestDoAction(zone, edx);
}

// WS_Loading.fng (the loading screen over the story movies) carries a developer's text object
// reading "EVENT HANDLER", drawn in red in the top right corner. Every frontend object is drawn
// through cFEngGameInterface::RenderObject (vtable slot): that string is left out.
constexpr uintptr_t kSlotRenderObject = 0x89B46C;
constexpr uintptr_t kRenderObject = 0x516630;         // cFEngGameInterface::RenderObject(FEObject*), thiscall
constexpr int kObjType = 0x18;                        // FEObject: 2 = FEString
constexpr int kStringText = 0x64;                     // FEString: FEWideString, +0 the characters

// The HUD's texts, for hiding one by its object (the location name): each new text is logged with
// its object's name hash (+0x10) and the string's label hash (FEString +0x60), and objects listed in
// [HUD] HideText (name hashes, hex, comma separated) aren't drawn.
constexpr int kObjNameHash = 0x10;
constexpr int kStringLabel = 0x60;

const std::set<uint32_t>& HiddenTexts() {
    static std::set<uint32_t> hidden;
    static bool read;
    if (!read) {
        read = true;
        char ini[MAX_PATH], buf[1024];
        ConfigFile("nfsmw_gameplay.ini", ini, false);
        GetPrivateProfileStringA("HUD", "HideText", "", buf, sizeof(buf), ini);
        for (char* t = strtok(buf, ", "); t; t = strtok(nullptr, ", ")) hidden.insert((uint32_t)strtoul(t, nullptr, 16));
        if (!hidden.empty()) Log("flow: %u HUD texts hidden ([HUD] HideText)", (unsigned)hidden.size());
    }
    return hidden;
}

void NoteText(uint8_t* obj, const wchar_t* text) {
    static std::set<uint64_t> seen;
    if (seen.size() >= 400 || !text[0]) return;
    uint32_t name = *(uint32_t*)(obj + kObjNameHash), label = *(uint32_t*)(obj + kStringLabel);
    if (!seen.insert(((uint64_t)name << 32) | label).second) return;
    char narrow[96];
    int n = 0;
    for (const wchar_t* c = text; *c && n < 90; c++) narrow[n++] = *c >= 32 && *c < 127 ? (char)*c : '?';
    narrow[n] = 0;
    Log("flow: HUD text object %08X label %08X: \"%s\"", name, label, narrow);
}

// HUD popups left out ([HUD] Infractions / CostToState / LocationNames = 0): the infractions list
// (Hud_Infractions, built at 0x5A1380) and the cost to state counter (Hud_CostToState, CTS_GROUP,
// built at 0x59D0C0) are HUD elements: each keeps the FE objects it drives in a list at +8
// (node: next, prev, object at +8). Once one is built, those objects and everything inside them
// are noted, and RenderObject leaves them out. The area name shown on entering a neighbourhood
// ("Ocean View") is a generic message: LocalPlayer (0x6EE360, free roam only) finds the zone
// trigger the car is in and, when it changes, requests the zone's name in its own message style
// (0xA19BB14C, used by nothing else). Those requests are dropped at
// GenericMessage::RequestGenericMessage (its vtable slot); every other message (totaled, busted,
// cops taken out) goes through.
constexpr uintptr_t kBuildInfractions = 0x5A1380, kCallBuildInfractions = 0x5A6B73;   // thiscall, 3 args
constexpr uintptr_t kBuildCostToState = 0x59D0C0, kCallBuildCostToState = 0x5A6A69;   // thiscall, 3 args
constexpr uintptr_t kSlotRequestMessage = 0x8A208C;  // GenericMessage's IGenericMessage vtable: RequestGenericMessage
constexpr uintptr_t kRequestMessage = 0x568030;
constexpr uint32_t kLocationStyle = 0xA19BB14C;
constexpr int kElemObjects = 0x08, kNodeObj = 0x08;
constexpr int kGroupChildren = 0x5C, kListHead = 0x08;

std::set<uint8_t*> s_hiddenInfractions, s_hiddenCts;

void NoteTree(std::set<uint8_t*>& out, uint8_t* obj, int depth) {
    if (!obj || depth > 8 || IsBadReadPtr(obj, 0x60) || !out.insert(obj).second) return;
    if (*(int*)(obj + kObjType) != 5) return;   // FE_Group: its children too
    for (uint8_t* c = *(uint8_t**)(obj + kGroupChildren + kListHead); c && out.size() < 512; c = *(uint8_t**)(c + 4))
        NoteTree(out, c, depth + 1);
}

void NoteElement(std::set<uint8_t*>& out, uint8_t* elem, const char* what) {
    out.clear();
    uint8_t* sentinel = elem + kElemObjects;
    int n = 0;
    for (uint8_t* node = *(uint8_t**)sentinel; node && node != sentinel && n < 64; node = *(uint8_t**)node, n++)
        NoteTree(out, *(uint8_t**)(node + kNodeObj), 0);
    Log("flow: %s hidden (%u HUD objects)", what, (unsigned)out.size());
}

bool __fastcall Hook_RequestMessage(void* gm, void* edx, const char* text, bool singleFrame, uint32_t style,
                                    uint32_t icon, uint32_t iconStyle, int priority) {
    if (style == kLocationStyle && g_cfg.hideLocationNames) return false;
    return ((bool(__fastcall*)(void*, void*, const char*, bool, uint32_t, uint32_t, uint32_t, int))kRequestMessage)(
        gm, edx, text, singleFrame, style, icon, iconStyle, priority);
}

void* __fastcall Hook_BuildInfractions(uint8_t* elem, void* edx, void* a, void* b, void* c) {
    void* r = ((void*(__fastcall*)(void*, void*, void*, void*, void*))kBuildInfractions)(elem, edx, a, b, c);
    if (g_cfg.hideInfractions) NoteElement(s_hiddenInfractions, elem, "infractions popup");
    else s_hiddenInfractions.clear();
    return r;
}

void* __fastcall Hook_BuildCostToState(uint8_t* elem, void* edx, void* a, void* b, void* c) {
    void* r = ((void*(__fastcall*)(void*, void*, void*, void*, void*))kBuildCostToState)(elem, edx, a, b, c);
    if (g_cfg.hideCostToState) NoteElement(s_hiddenCts, elem, "cost to state popup");
    else s_hiddenCts.clear();
    return r;
}

void __fastcall Hook_RenderObject(void* fe, void* edx, uint8_t* obj) {
    if (obj && (s_hiddenInfractions.count(obj) || s_hiddenCts.count(obj))) return;
    if (obj && *(int*)(obj + kObjType) == 2) {
        const wchar_t* text = *(const wchar_t**)(obj + kStringText);
        if (text && !IsBadReadPtr(text, 2)) {
            if (!IsBadReadPtr(text, 28) && wcscmp(text, L"EVENT HANDLER") == 0) return;
            NoteText(obj, text);
            if (HiddenTexts().count(*(uint32_t*)(obj + kObjNameHash))) return;
        }
    }
    ((void(__fastcall*)(void*, void*, uint8_t*))kRenderObject)(fe, edx, obj);
}

// The EA Trax "now playing" banner (the Chyron screen): SummonChyron (cdecl) is its only way in,
// from FEManager::Update and the EA Trax sound object; it returns at once.
constexpr uintptr_t kSummonChyron = 0x595ED0;

void HideSongBanner() {
    if (!Expect(kSummonChyron, {0xA1, 0x48, 0xE3, 0x8E, 0x00})) {
        Log("flow: song banner code not as expected, left on");
        return;
    }
    DWORD old;
    VirtualProtect((void*)kSummonChyron, 1, PAGE_EXECUTE_READWRITE, &old);
    *(uint8_t*)kSummonChyron = 0xC3;   // ret
    VirtualProtect((void*)kSummonChyron, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)kSummonChyron, 1);
    Log("flow: song banner (EA Trax now playing) hidden");
}

// Race start: the car handed back after the countdown.
//
// Rolling starts hand the player's car to the AI at the start: the event EBecomeAiCar or the
// script's Game.ForceAIControl sets IHumanAI::SetAiControl(true). With a countdown, GO (the HUD's
// MCountdownDone) is answered by Game.StartRaceTimers and the event EBecomePlayerCar, which clears
// the AI control and gives the controls back. The Challenge Series' time trials (toll booths) have
// no countdown: the script calls ForceAIControl and StartRaceTimers in the same frame, and nothing
// ever clears the AI control, so the AI drove the whole race (seen in the log, never a
// ClearAIControl or EBecomePlayerCar). The game stops reading the player's input meanwhile.
// So when the AI took the car within kAiAroundGo of GO (either side), EBecomePlayerCar's own handler
// is run here as soon as the player presses a pedal or steers, and kHandBackAfter after GO at
// the latest. A car the AI takes over later (a finished race) is left alone.
// Every step of the hand-over is logged.
constexpr uintptr_t kBecomeAiCar = 0x640120;          // EBecomeAiCar's handler (its destructor), thiscall
constexpr uintptr_t kCallBecomeAiCar = 0x643A73;
constexpr uintptr_t kBecomePlayerCar = 0x640250;      // EBecomePlayerCar's handler, thiscall; reads nothing of the event
constexpr uintptr_t kCallBecomePlayerCar = 0x643AA3;
constexpr uintptr_t kCountdownDoneMsg = 0x567970;     // MCountdownDone::MCountdownDone(), thiscall
constexpr uintptr_t kCallsCountdownDone[] = {0x567A94, 0x567CBE};   // the HUD countdown at GO (two clocks)
constexpr uintptr_t kStartRaceTimers = 0x604EF0;      // Lua Game.StartRaceTimers, cdecl
constexpr uintptr_t kForceAIControl = 0x6123B0;       // Lua Game.ForceAIControl(player), cdecl
constexpr uintptr_t kClearAIControl = 0x612420;       // Lua Game.ClearAIControl(player), cdecl
constexpr uintptr_t kIHumanAIHandle = 0x403770;       // IHumanAI::_IHandle; vtbl+0xC GetAiControl, +0x10 SetAiControl
constexpr uintptr_t kIInputPlayerHandle = 0x403F80;   // IInputPlayer::_IHandle; vtbl+4 BlockInput(bool), +8 IsBlocked
constexpr uintptr_t kSimTime = 0x9885D8;              // float: game time (Sim::GetTime), stops while paused
constexpr uintptr_t kRaceStatus = 0x91E000;           // GRaceStatus*: +0x1960 mPlayMode (0 = roaming)
constexpr float kHandBackAfter = 3.0f;                // seconds after GO at the latest
constexpr float kHandBackUntil = 30.0f;
constexpr float kAiAroundGo = 2.0f;                   // AI take-over this close to GO is the race start's

int(__cdecl* s_startRaceTimers)(void*);
void(__cdecl* s_forceAIControl)(int);
void(__cdecl* s_clearAIControl)(int);

struct {
    float goAt = -1;        // game time of the last GO (MCountdownDone or StartRaceTimers), -1 = none pending
    bool ai = false;        // the player's car was AI-driven at the last step
    float aiSince = 0;      // ... since this game time
    float stuckSince = -1;  // free roam: AI-driven, standing still and the throttle held since, -1 = not
} s_start;

// Free roam: after a race the game hands the player's car to the AI and normally takes it back on
// the way out; when it does not, the car sits still and ignores the controls until the next race
// start (seen after beating a Blacklist rival, 2026-10-20: it even carried over to another car from
// the safehouse). In free roam, a car the AI has had for kFreeAiGrace, standing still while the
// throttle has been held for kFreeStuckHeld, is handed back like a race start's.
constexpr float kFreeAiGrace = 10.0f;
constexpr float kFreeStuckHeld = 2.0f;
constexpr float kFreeStuckSpeed = 1.0f;     // m/s

float SimTime() { return *(float*)kSimTime; }

const char* RaceState() {
    uint8_t* race = *(uint8_t**)kRaceStatus;
    return !race ? "no race status" : *(int*)(race + 0x1960) ? "racing" : "roaming";
}

void* PlayerHumanAI() { return Game_PlayerInterface((void*)kIHumanAIHandle); }

bool AiControl(void* ai) {
    return ((bool(__fastcall*)(void*, void*))(*(void***)ai)[3])(ai, nullptr);
}

void NoteGo(const char* by) {
    void* ai = PlayerHumanAI();
    Log("race start: GO (%s) at %.2f, %s, the player's car %s", by, SimTime(), RaceState(),
        !ai ? "not found" : AiControl(ai) ? "still AI-driven" : "under your control");
    s_start.goAt = SimTime();
}

void __fastcall Hook_BecomeAiCar(void* ev, void* edx) {
    Log("race start: EBecomeAiCar at %.2f (%s): the AI drives the player's car", SimTime(), RaceState());
    s_start.goAt = -1;
    ((void(__fastcall*)(void*, void*))kBecomeAiCar)(ev, edx);
}

void __fastcall Hook_BecomePlayerCar(void* ev, void* edx) {
    Log("race start: EBecomePlayerCar at %.2f (%s): the car back to the player", SimTime(), RaceState());
    ((void(__fastcall*)(void*, void*))kBecomePlayerCar)(ev, edx);
}

void* __fastcall Hook_CountdownDoneMsg(void* msg, void* edx) {
    NoteGo("HUD countdown");
    return ((void*(__fastcall*)(void*, void*))kCountdownDoneMsg)(msg, edx);
}

int __cdecl Hook_StartRaceTimers(void* L) {
    NoteGo("race script starts the timers");
    return s_startRaceTimers(L);
}

void __cdecl Hook_ForceAIControl(int player) {
    Log("race start: script Game.ForceAIControl(%d) at %.2f (%s)", player, SimTime(), RaceState());
    s_forceAIControl(player);
}

void __cdecl Hook_ClearAIControl(int player) {
    Log("race start: script Game.ClearAIControl(%d) at %.2f (%s)", player, SimTime(), RaceState());
    s_clearAIControl(player);
}

// Retargets the call at `site` to `hook` if it still calls `target`.
bool RetargetCall(uintptr_t site, uintptr_t target, void* hook) {
    if (*(uint8_t*)site != 0xE8 || site + 5 + *(int32_t*)(site + 1) != target) return false;
    int32_t rel = (int32_t)((uintptr_t)hook - (site + 5));
    DWORD old;
    VirtualProtect((void*)(site + 1), 4, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)(site + 1), &rel, 4);
    VirtualProtect((void*)(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)site, 5);
    return true;
}

void RaceStart_Install() {
    // Hand-back first: without it the logging alone would be no fix.
    if (!Expect(kBecomePlayerCar, {0x6A, 0xFF, 0x68}) || !RetargetCall(kCallBecomePlayerCar, kBecomePlayerCar, (void*)&Hook_BecomePlayerCar)) {
        Log("flow: EBecomePlayerCar code not as expected, race start hand-back unavailable");
        return;
    }
    if (!RetargetCall(kCallBecomeAiCar, kBecomeAiCar, (void*)&Hook_BecomeAiCar))
        Log("flow: EBecomeAiCar call not as expected, not logged");
    int go = 0;
    for (uintptr_t site : kCallsCountdownDone) go += RetargetCall(site, kCountdownDoneMsg, (void*)&Hook_CountdownDoneMsg);
    // mov ecx, [GRaceStatus::fObj]: an absolute address, moves as it is.
    if (Expect(kStartRaceTimers, {0x8B, 0x0D, 0x00, 0xE0, 0x91, 0x00}) &&
        (s_startRaceTimers = (int(__cdecl*)(void*))Detour(kStartRaceTimers, 6, (void*)&Hook_StartRaceTimers)))
        go++;
    // mov eax, [esp+4]; test eax, eax
    if (Expect(kForceAIControl, {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0}))
        s_forceAIControl = (void(__cdecl*)(int))Detour(kForceAIControl, 6, (void*)&Hook_ForceAIControl);
    if (Expect(kClearAIControl, {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0}))
        s_clearAIControl = (void(__cdecl*)(int))Detour(kClearAIControl, 6, (void*)&Hook_ClearAIControl);
    Log("flow: race start hand-back installed (%d of 3 GO signals)", go);
}

}  // namespace

// EBecomePlayerCar's own handler, and the player's input unblocked if it was left blocked.
void HandBackToPlayer(void* ai, const char* who) {
    uint8_t event[16] = {};
    ((void(__fastcall*)(void*, void*))kBecomePlayerCar)(event, nullptr);
    if (AiControl(ai)) Log("%s: the car is still AI-driven (game mode flag set?)", who);
    // The player's input left blocked would keep the car still: unblocked too (vtbl+4 BlockInput, +8 IsBlocked).
    void* input = Game_PlayerInterface((void*)kIInputPlayerHandle);
    if (input && ((bool(__fastcall*)(void*, void*))(*(void***)input)[2])(input, nullptr)) {
        Log("%s: the player's input was blocked, unblocked", who);
        ((void(__fastcall*)(void*, void*, bool))(*(void***)input)[1])(input, nullptr, false);
    }
}

// See kFreeAiGrace.
void FreeRoamHandBack(void* ai, bool on, float now) {
    uint8_t* race = *(uint8_t**)kRaceStatus;
    bool roaming = race && *(int*)(race + 0x1960) == 0;
    InputSnapshot in = Input_Get();
    bool throttle = in.pedalBound[0] && in.pedal[0] > 0.5f;
    bool stuck = on && roaming && s_start.goAt < 0 && now - s_start.aiSince > kFreeAiGrace && throttle &&
                 Physics_PlayerSpeed() < kFreeStuckSpeed;
    if (!stuck) {
        s_start.stuckSince = -1;
        return;
    }
    if (s_start.stuckSince < 0) s_start.stuckSince = now;
    if (now - s_start.stuckSince < kFreeStuckHeld) return;
    s_start.stuckSince = -1;
    Log("free roam: the player's car AI-driven for %.0f s, standing still with the throttle held: EBecomePlayerCar run here",
        now - s_start.aiSince);
    HandBackToPlayer(ai, "free roam");
}

void Flow_PlayerStep() {
    void* ai = PlayerHumanAI();
    bool on = ai && AiControl(ai);
    float now = SimTime();
    if (on != s_start.ai) {
        s_start.ai = on;
        s_start.aiSince = now;
        Log("race start: the player's car %s at %.2f (%s)", on ? "AI-driven" : "under your control", now, RaceState());
    }
    if (ai) FreeRoamHandBack(ai, on, now);
    if (!on || s_start.goAt < 0) return;
    if (s_start.aiSince > s_start.goAt + kAiAroundGo) {   // the AI took over well after GO: a finished race
        s_start.goAt = -1;
        return;
    }
    float late = now - s_start.goAt;
    if (late < 0.0f) return;
    InputSnapshot in = Input_Get();
    bool touched = (in.pedalBound[0] && in.pedal[0] > 0.2f) || (in.pedalBound[1] && in.pedal[1] > 0.2f) ||
                   (in.steerBound && fabsf(in.steer) > 0.15f);
    if (!touched && late < kHandBackAfter) return;
    s_start.goAt = -1;
    if (late > kHandBackUntil) return;
    Log("race start: still AI-driven %.1f s after GO (%s), the game didn't hand it back: EBecomePlayerCar run here",
        late, touched ? "you took the controls" : "time");
    HandBackToPlayer(ai, "race start");
}

void Flow_Install() {
    HideSongBanner();
    if (!RetargetCall(kCallBuildInfractions, kBuildInfractions, (void*)&Hook_BuildInfractions))
        Log("flow: unexpected HUD code, infractions popup left");
    if (!RetargetCall(kCallBuildCostToState, kBuildCostToState, (void*)&Hook_BuildCostToState))
        Log("flow: unexpected HUD code, cost to state popup left");
    if (*(uintptr_t*)kSlotRequestMessage == kRequestMessage) {
        void* p = (void*)&Hook_RequestMessage;
        DWORD old;
        VirtualProtect((void*)kSlotRequestMessage, 4, PAGE_READWRITE, &old);
        memcpy((void*)kSlotRequestMessage, &p, 4);
        VirtualProtect((void*)kSlotRequestMessage, 4, old, &old);
    } else {
        Log("flow: unexpected generic message code, area names left");
    }
    RaceStart_Install();
    if (*(uintptr_t*)kSlotRenderObject == kRenderObject) {
        void* p = (void*)&Hook_RenderObject;
        DWORD old;
        VirtualProtect((void*)kSlotRenderObject, 4, PAGE_READWRITE, &old);
        memcpy((void*)kSlotRenderObject, &p, 4);
        VirtualProtect((void*)kSlotRenderObject, 4, old, &old);
        Log("flow: loading screen's \"EVENT HANDLER\" text hidden");
    } else {
        Log("flow: unexpected frontend renderer, \"EVENT HANDLER\" text left");
    }
    // push -1; push <handler> / mov eax, fs:[0]: both move as they are.
    if (!Expect(kZoneRequestDoAction, {0x6A, 0xFF, 0x68}) || !Expect(kDoSafeHouseIntro, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00})) {
        Log("flow: safehouse code not as expected, first-visit fix unavailable");
        return;
    }
    s_doSafeHouseIntro = (void(__cdecl*)(void*))Detour(kDoSafeHouseIntro, 6, (void*)&Hook_DoSafeHouseIntro);
    if (!s_doSafeHouseIntro) return;
    s_requestDoAction = (void(__fastcall*)(void*, void*))Detour(kZoneRequestDoAction, 7, (void*)&Hook_RequestDoAction);
    // mov eax, [SkipFE]: an absolute address, moves as it is.
    if (Expect(kSkipCareerIntro, {0xA1, 0x64, 0x60, 0x92, 0x00}))
        s_skipCareerIntro = (bool(__cdecl*)())Detour(kSkipCareerIntro, 5, (void*)&Hook_SkipCareerIntro);
    else
        Log("flow: Game_SkipCareerIntro not as expected, the career's own intro trigger left as it is");
    Log("flow: installed (first safehouse visit)");
}
