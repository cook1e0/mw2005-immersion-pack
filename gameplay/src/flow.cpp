// The frontend flow around story movies (the first safehouse visit's Mia movie).
//
// Entering the safehouse the first time (Game_DoSafeHouseIntro) sets the movie storyfmv_saf25 and
// the screen after it, SafeHouseRivalBio.fng (FEAnyMovieScreen's ReturnToPackageName), and quits
// to the frontend on FEAnyMovie.fng. FEAnyMovieScreen::DismissMovie runs when the movie ends and
// on accept / start: it switches to ReturnToPackageName and clears it. Dismissed a second time
// before the movie screen is gone (the movie's end and a button press, a held or repeated accept)
// it finds the name empty and pops the package stack instead, which drops to the main menu, and
// the career never gets its first safehouse visit. That second dismissal is ignored here.
//
// The flow is logged as well: the quit to the frontend, package switches and pops, the movie
// screen's messages and dismissals.
#include "flow.h"
#include "common.h"
#include <cstring>

namespace {

constexpr uintptr_t kDoSafeHouseIntro = 0x605560;   // Game_DoSafeHouseIntro(GRuntimeInstance*)
constexpr uintptr_t kQuitToFE = 0x623F80;           // EQuitToFE: runs the event (ecx = event)
constexpr uintptr_t kQueueSwitch = 0x525940;        // cFEng::QueuePackageSwitch(name, arg, ctrl, bool)
constexpr uintptr_t kQueuePop = 0x5258B0;           // cFEng::QueuePackagePop(count)
constexpr uintptr_t kMovieCtor = 0x54EDA0;          // FEAnyMovieScreen::FEAnyMovieScreen(ScreenConstructorData*)
constexpr uintptr_t kMovieMessage = 0x559880;       // FEAnyMovieScreen::NotificationMessage(msg, obj, p1, p2)
constexpr uintptr_t kDismissMovie = 0x542940;       // FEAnyMovieScreen::DismissMovie()
constexpr uintptr_t kReturnToPackage = 0x91C270;    // FEAnyMovieScreen::ReturnToPackageName[64]
constexpr uintptr_t kMovieName = 0x8F3750;          // FEAnyMovieScreen::MovieFilename[64]
constexpr uintptr_t kGameState = 0x925E90;          // the game flow's state (6 = in game)
constexpr uintptr_t kRaceStatus = 0x91E000;         // GRaceStatus*: +0x1964 the race context (2 = career)
constexpr uintptr_t kFEDatabase = 0x91CF90;         // cFrontendDatabase*: +0x12C flags (bit 17: post-rival mode), +0x10 the player's settings
constexpr uintptr_t kJumpToSafeHouse = 0x6052B0;    // Game_JumpToSafeHouse() (career script): quit to MainMenu_Sub.fng
constexpr uintptr_t kZoneAction = 0x58C640;         // MenuZoneTrigger::RequestDoAction (driving into a safehouse / car lot / shop)
constexpr uintptr_t kSkipCareerIntro = 0x612490;    // bool Game_SkipCareerIntro() (career script)
constexpr uintptr_t kSkipFE = 0x926064;             // SkipFE config
constexpr uintptr_t kSkipCareerIntroCheat = 0x926125, kSkipDDayCheat = 0x926126;

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

const char* Str(const char* s) { return s && !IsBadStringPtrA(s, 64) ? s : "?"; }

int RaceContext() {
    uint8_t* race = *(uint8_t**)kRaceStatus;
    return race ? *(int*)(race + 0x1964) : -1;
}

void(__cdecl* s_doSafeHouseIntro)(void*);
void(__cdecl* s_jumpToSafeHouse)();
void(__fastcall* s_zoneAction)(void*, void*);
bool(__cdecl* s_skipCareerIntro)();
void(__fastcall* s_quitToFE)(void*, void*);
void(__fastcall* s_queueSwitch)(void*, void*, const char*, int, unsigned, int);
void(__fastcall* s_queuePop)(void*, void*, int);
void*(__fastcall* s_movieCtor)(void*, void*, void*);
void(__fastcall* s_movieMessage)(void*, void*, unsigned, void*, unsigned, unsigned);
void(__cdecl* s_dismissMovie)();

// Now() of the dismissal that switched to the movie's next screen, while its screen is still up.
double s_dismissedAt = -1;

// The career's progress as the career script sees it: the settings' bin (+0xB0: 16 at the start of
// the blacklist) and flags (+0xAC: bit 5 career intro done, bit 14), the frontend flags.
void LogCareer(const char* when) {
    uint8_t* db = *(uint8_t**)kFEDatabase;
    uint8_t* career = db ? *(uint8_t**)(db + 0x10) : nullptr;
    Log("flow: %s: career bin %d, flags 0x%X, frontend flags 0x%X, SkipFE %d, cheats %d %d, race context %d", when,
        career ? career[0xB0] : -1, career ? *(uint32_t*)(career + 0xAC) : 0, db ? *(uint32_t*)(db + 0x12C) : 0,
        *(int*)kSkipFE, *(uint8_t*)kSkipCareerIntroCheat, *(uint8_t*)kSkipDDayCheat, RaceContext());
}

void __cdecl Hook_JumpToSafeHouse() {
    LogCareer("career script jumps to the safehouse menu");
    s_jumpToSafeHouse();
}

void __fastcall Hook_ZoneAction(uint8_t* zone, void* edx) {
    const char* type = *(const char**)(zone + 0x14);
    LogCareer(type && !IsBadStringPtrA(type, 32) ? type : "menu zone");
    s_zoneAction(zone, edx);
}

bool __cdecl Hook_SkipCareerIntro() {
    bool skip = s_skipCareerIntro();
    static int logged;
    if (logged < 20) {
        logged++;
        Log("flow: career script asks whether to skip the career intro: %s", skip ? "yes" : "no");
    }
    return skip;
}

void __cdecl Hook_DoSafeHouseIntro(void* instance) {
    Log("flow: first safehouse visit: the intro movie, then SafeHouseRivalBio.fng");
    s_doSafeHouseIntro(instance);
}

void __fastcall Hook_QuitToFE(uint8_t* ev, void* edx) {
    LogCareer("quit to the frontend");
    Log("flow: quit to the frontend (garage type %d, package %s, game state %d, race context %d)", *(int*)(ev + 8),
        *(const char**)(ev + 0xC) ? Str(*(const char**)(ev + 0xC)) : "none", *(int*)kGameState, RaceContext());
    s_quitToFE(ev, edx);
}

void __fastcall Hook_QueueSwitch(void* fe, void* edx, const char* name, int arg, unsigned ctrl, int flag) {
    Log("flow: package switch to %s", Str(name));
    s_queueSwitch(fe, edx, name, arg, ctrl, flag);
}

void __fastcall Hook_QueuePop(void* fe, void* edx, int count) {
    Log("flow: package pop (%d)", count);
    s_queuePop(fe, edx, count);
}

void* __fastcall Hook_MovieCtor(void* screen, void* edx, void* data) {
    s_dismissedAt = -1;
    Log("flow: movie screen for %s (then %s)", Str((const char*)kMovieName),
        *(const char*)kReturnToPackage ? Str((const char*)kReturnToPackage) : "back");
    return s_movieCtor(screen, edx, data);
}

void __fastcall Hook_MovieMessage(void* screen, void* edx, unsigned msg, void* obj, unsigned p1, unsigned p2) {
    if (msg == 0x406415E3 || msg == 0xB5AF2461 || msg == 0xC3960EB9)
        Log("flow: movie screen: %s", msg == 0xC3960EB9 ? "movie finished" : msg == 0x406415E3 ? "accept" : "start");
    s_movieMessage(screen, edx, msg, obj, p1, p2);
}

void __cdecl Hook_DismissMovie() {
    const char* next = (const char*)kReturnToPackage;
    if (!next[0] && s_dismissedAt >= 0 && Now() - s_dismissedAt < 10.0) {
        Log("flow: movie dismissed again before its screen closed: ignored (it would drop to the main menu)");
        return;
    }
    uint8_t* db = *(uint8_t**)kFEDatabase;
    bool rival = db && (*(uint32_t*)(db + 0x12C) >> 17 & 1);
    Log("flow: movie dismissed: %s", next[0] ? Str(next) : rival ? "the rival flow" : "back (package pop)");
    if (next[0]) s_dismissedAt = Now();
    s_dismissMovie();
}

}  // namespace

void Flow_Install() {
    struct Site { uintptr_t at; std::initializer_list<uint8_t> bytes; void* hook; void** orig; const char* what; };
    const Site sites[] = {
        {kDoSafeHouseIntro, {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00}, (void*)&Hook_DoSafeHouseIntro, (void**)&s_doSafeHouseIntro, "safehouse intro"},
        {kQuitToFE, {0x6A, 0xFF, 0x68}, (void*)&Hook_QuitToFE, (void**)&s_quitToFE, "quit to frontend"},
        {kQueueSwitch, {0x83, 0x3D, 0x90, 0x5E, 0x92, 0x00, 0x06}, (void*)&Hook_QueueSwitch, (void**)&s_queueSwitch, "package switch"},
        {kQueuePop, {0x56, 0x57, 0x8B, 0xF1, 0x8B, 0x0E}, (void*)&Hook_QueuePop, (void**)&s_queuePop, "package pop"},
        {kMovieCtor, {0x6A, 0xFF, 0x68}, (void*)&Hook_MovieCtor, (void**)&s_movieCtor, "movie screen"},
        {kMovieMessage, {0x56, 0x57, 0x8B, 0x7C, 0x24, 0x0C}, (void*)&Hook_MovieMessage, (void**)&s_movieMessage, "movie messages"},
        {kDismissMovie, {0xA0, 0x70, 0xC2, 0x91, 0x00}, (void*)&Hook_DismissMovie, (void**)&s_dismissMovie, "movie dismissal"},
        {kJumpToSafeHouse, {0x6A, 0xFF, 0x68}, (void*)&Hook_JumpToSafeHouse, (void**)&s_jumpToSafeHouse, "jump to safehouse"},
        {kZoneAction, {0x6A, 0xFF, 0x68}, (void*)&Hook_ZoneAction, (void**)&s_zoneAction, "menu zone action"},
        {kSkipCareerIntro, {0xA1, 0x64, 0x60, 0x92, 0x00}, (void*)&Hook_SkipCareerIntro, (void**)&s_skipCareerIntro, "career intro check"},
    };
    const size_t stolen[] = {6, 7, 7, 6, 7, 6, 5, 7, 7, 5};
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        const Site& s = sites[i];
        if (!Expect(s.at, s.bytes)) {
            Log("flow: %s not as expected, not hooked", s.what);
            continue;
        }
        *s.orig = Detour(s.at, stolen[i], s.hook);
        if (!*s.orig) Log("flow: %s not hooked", s.what);
    }
    Log("flow: installed (movie dismissal guard, frontend log)");
}
