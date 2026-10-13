// Exercise the production seat hook against a small, synthetic v1.3 memory layout.
// No game process, input injection or headset is used. Padding reserves the game's addresses.
#include "../src/cockpit.cpp"
#include <stdlib.h>

Config g_cfg;
volatile uint8_t gameAddressPadding[0x600000];
static double testTime = 1;
void Log(const char*, ...) {}
void ConfigFile(const char*, char* out, bool) { strcpy(out, "missing-cockpit-test.ini"); }

static uint8_t connData[0x500], anchorData[0xD0], moverData[16];
static void* moverVtbl[8];
static M4 physicsBody;
static uint8_t* loadedConn;

static uint8_t* __cdecl FindTestConn(uint32_t id) {
    return loadedConn && At<uint32_t>(loadedConn, kConnWorldId) == id ? loadedConn : nullptr;
}
static uint8_t* __fastcall GetTestAnchor(void*, void*) { return anchorData; }

static void Check(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void Near(float a, float b, const char* message) { Check(fabsf(a - b) < 0.0002f, message); }
static M4 Identity() {
    M4 m = {};
    for (int i = 0; i < 4; i++) m.m[i][i] = 1;
    return m;
}
static void Stub(uintptr_t address, const void* function) {
    DWORD old;
    Check(VirtualProtect((void*)address, 5, PAGE_EXECUTE_READWRITE, &old) != 0,
          "make synthetic function executable");
    uint8_t* code = (uint8_t*)address;
    code[0] = 0xE9;
    *(int32_t*)(code + 1) = (int32_t)((uintptr_t)function - address - 5);
    FlushInstructionCache(GetCurrentProcess(), code, 5);
}
static M4 Apply() {
    M4 view = Identity();
    bool applied = Cockpit_ApplySeat(view);
    if (!applied) {
        auto find = (uint8_t*(__cdecl*)(uint32_t))kFindConn;
        fprintf(stderr, "seat=%d valid=%d id=%u now=%.1f age=%.1f anchor=%p conn=%p lookup=%p\n",
                g_cfg.seat, s_head.valid, s_head.worldId, Now(), Now()-s_head.time,
                ViewAnchor((uint8_t*)kPlayerView), loadedConn, find(s_head.worldId));
    }
    Check(applied, "seat applies to loaded player car");
    return MInvRigid(view);
}

int main() {
    testTime = Now();
    static_assert(sizeof(void*) == 4, "test the game's 32-bit layout");
    Check((uintptr_t)gameAddressPadding < kFindConn &&
          (uintptr_t)gameAddressPadding + sizeof(gameAddressPadding) > kPlayerView + 0x70,
          "game-address fixtures lie within test padding");
    Stub(kFindConn, (void*)&FindTestConn);
    loadedConn = connData;
    At<uintptr_t>(connData, 0) = kCarRenderConnVtbl;
    At<uint32_t>(connData, kConnWorldId) = 7;
    At<const M4*>(connData, kConnBodyMatrix) = &physicsBody;
    physicsBody = Identity();
    At<M4>(connData, kConnRenderMatrix) = Identity();
    At<uint32_t>(anchorData, kAnchorWorldId) = 7;
    At<int16_t>(anchorData, kAnchorPov) = 1;
    moverVtbl[kMoverGetAnchor] = (void*)&GetTestAnchor;
    At<void**>(moverData, 0) = moverVtbl;
    At<uint8_t*>((void*)kPlayerView, kViewMovers) = moverData + 4;
    g_cfg.hideHead = false;
    s_head.valid = true;
    s_head.time = testTime;
    s_head.worldId = 7;
    s_head.car = s_offsetCar = "TEST";
    s_head.body[0] = 0.2f; s_head.body[1] = 0.4f; s_head.body[2] = 1.1f;

    // Translate and rotate the car independently of the original camera: no previous-frame lag.
    physicsBody.m[0][0] = 0; physicsBody.m[0][1] = 1;
    physicsBody.m[1][0] = -1; physicsBody.m[1][1] = 0;
    physicsBody.m[3][0] = 100; physicsBody.m[3][1] = 200;
    s_offset[0] = 0.1f; s_offset[1] = 0.2f; s_offset[2] = 0.3f;
    M4 w = Apply();
    Near(w.m[3][0], 99.7f, "right offset follows rotated car");
    Near(w.m[3][1], 200.5f, "forward offset follows rotated car");
    Near(w.m[3][2], 1.3f, "up offset follows car");
    Near(w.m[2][1], 1, "camera heading follows car");
    At<float>(connData, kConnCog) = 0.5f;
    At<float>(connData, kConnCog + 4) = 0.1f;
    At<float>(connData, kConnCog + 8) = 0.2f;
    w = Apply();
    Near(w.m[3][0], 99.8f, "centre of mass offset is rotated into world space");
    Near(w.m[3][1], 200.0f, "centre of mass forward offset");
    Near(w.m[3][2], 1.1f, "centre of mass vertical offset");
    memset(connData + kConnCog, 0, 16);
    physicsBody.m[3][0] += 20;
    w = Apply();
    Near(w.m[3][0], 119.7f, "seat follows current frame's body");

    // A camera switch must choose the transform used by this draw, not last frame's POV mode.
    At<int16_t>(anchorData, kAnchorPov) = 2;
    At<M4>(connData, kConnRenderMatrix).m[3][0] = 300;
    w = Apply();
    Near(w.m[3][0], 300.5f, "hood camera selects current render transform");

    // An unloaded/replaced car or a different anchor must never use a cached connection pointer.
    loadedConn = nullptr;
    M4 view = Identity();
    Check(!Cockpit_ApplySeat(view), "unloaded car releases seat safely");
    loadedConn = connData;
    At<uint32_t>(anchorData, kAnchorWorldId) = 8;
    Check(!Cockpit_ApplySeat(view), "different anchor releases seat");
    At<uint32_t>(anchorData, kAnchorWorldId) = 7;

    // Driver LOD changes refresh observations but cannot move an established seat in body space.
    s_frame.dist = 1; s_frame.worldId = 7; s_frame.car = "TEST";
    s_frame.body[0] = 9; s_frame.body[1] = 9; s_frame.body[2] = 9;
    Cockpit_EndFrame();
    Near(s_head.body[0], 0.2f, "LOD change preserves fixed seat");
    Check(!s_haveBase && !s_seatedConn && s_frame.dist > 1e8f, "frame state cleared");
    s_frame.dist = 1; s_frame.worldId = 8; s_frame.car = "OTHER";
    s_frame.body[0] = 0.8f;
    Cockpit_EndFrame();
    Near(s_head.body[0], 0.8f, "new car measures a new seat");
    s_head.time -= 1;
    Check(!Cockpit_ApplySeat(view), "expired observation releases seat");
    puts("PASS: cockpit motion, offsets, current POV, unload, anchor switch, LOD and frame lifecycle");
}
