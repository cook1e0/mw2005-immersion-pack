// Exercise the production seat hook against a small, synthetic v1.3 memory layout.
// No game process, input injection or headset is used. Padding reserves the game's addresses.
#include "../src/cockpit.cpp"
#include <stdlib.h>

Config g_cfg;
volatile uint8_t gameAddressPadding[0x600000];
static double testTime = 1;
void Log(const char*, ...) {}
void ConfigFile(const char*, char* out, bool) { strcpy(out, "missing-cockpit-test.ini"); }
void D3D_AddMirror(void*, const float*, int, const float*, int) {}
void D3D_RegisterMirrorGlass(void*, int, int, const float*, const uint16_t*, int) {}
void D3D_SetGlassCorners(void*, int, const float*) {}
void D3D_AddMirrorGlassPart(void*, int, const float*, const uint16_t*, int) {}
void D3D_RegisterWheelDraw(void*, int) {}
void D3D_AddDash(void*, const float*, const float*, const float*, const float*) {}
void D3D_AddColorTris(void*, const float*, int, uint32_t) {}

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

// Index buffer stand-in for the head reordering (Lock/Unlock/GetDesc only).
struct FakeIb : IDirect3DIndexBuffer9 {
    uint16_t data[4096];
    UINT size = 12;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, CONST void*, DWORD, DWORD) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, void*, DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID) override { return E_NOTIMPL; }
    DWORD STDMETHODCALLTYPE SetPriority(DWORD) override { return 0; }
    DWORD STDMETHODCALLTYPE GetPriority() override { return 0; }
    void STDMETHODCALLTYPE PreLoad() override {}
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_INDEXBUFFER; }
    HRESULT STDMETHODCALLTYPE Lock(UINT offset, UINT, void** p, DWORD) override { *p = (uint8_t*)data + offset; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Unlock() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DINDEXBUFFER_DESC* d) override {
        memset(d, 0, sizeof(*d));
        d->Format = D3DFMT_INDEX16;
        d->Size = size;
        return D3D_OK;
    }
};

// A driver model streamed out and another loaded at the same address must be prepared again, and
// the head-less entry must carry the live entry's vertex buffer, never one copied at first sight.
static void TestReloadedDriver() {
    static uint8_t solid[0x100], plat[0x40], entry[kEntrySize], format[16], camera[0x50];
    static float verts[18] = {0, 0, 0, 1, 0, 0, 0, 1, 0, -1, 0, 1.9f, -1, 1, 1.9f, -0.9f, 0, 2.0f};
    static uint16_t indices[6];
    static FakeIb ib;
    const uint16_t original[6] = {3, 4, 5, 0, 1, 2};    // head triangle first
    memcpy(indices, original, sizeof(indices));
    memcpy(ib.data, original, sizeof(indices));
    strcpy((char*)solid + kSolidName, "TESTCAR_KIT00_DRIVER_A");
    At<float>(solid, kSolidMin) = -1;
    At<float>(solid, kSolidMax + 8) = 2.0f;
    At<uint8_t*>(solid, kSolidPlat) = plat;
    At<int>(plat, kPlatNumEntries) = 1;
    At<uint8_t*>(plat, kPlatEntries) = entry;
    At<uint16_t*>(plat, kPlatIndices) = indices;
    At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) = &ib;
    At<int>(plat, kPlatNumIndices) = 6;
    At<int>(format, 8) = 12;
    At<uint8_t*>(entry, kEntryFormat) = format;
    At<int>(entry, kEntryTris) = 2;
    At<int>(entry, kEntryFirstIndex) = 0;
    At<float*>(entry, kEntryVertices) = verts;
    At<int>(entry, kEntryVertexBytes) = sizeof(verts);
    At<uintptr_t>(entry, 0x54) = 0x1111;                // vertex buffer

    Driver* first = GetDriver(solid);
    Check(first && first->ok && first->body[0] == 1 && first->heads[0] == 1 && indices[0] == 0 && ib.data[3] == 3, "driver head reordered last");
    Check(GetDriver(solid) == first, "unchanged driver stays cached");

    // Reloaded in place: same pointers, original triangle order and a new vertex buffer.
    memcpy(indices, original, sizeof(indices));
    memcpy(ib.data, original, sizeof(indices));
    At<uintptr_t>(entry, 0x54) = 0x2222;
    Driver* second = GetDriver(solid);
    Check(second && second != first && second->ok && indices[0] == 0 && s_retired.size() == 1, "reloaded driver prepared again");

    // The head next to the eyes: the entry drawn without it is a fresh copy of the live one.
    float model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    uint8_t modelData[0x10] = {};
    At<uint8_t*>(modelData, kModelSolid) = solid;
    memcpy(camera + kCamPosition, second->head, sizeof(second->head));
    void* savedCam = At<void*>((void*)kPlayerView, kViewCamera);
    At<void*>((void*)kPlayerView, kViewCamera) = camera;
    g_cfg.hideHead = true;
    uint8_t* trimmed = OnSubmit(modelData, model, entry);
    Check(trimmed && trimmed != entry && s_driverParts == 1 && At<int>(trimmed, kEntryTris) == 1 &&
              At<uintptr_t>(trimmed, 0x54) == 0x2222,
          "head-less entry uses the live vertex buffer");
    g_cfg.hideHead = false;
    At<void*>((void*)kPlayerView, kViewCamera) = savedCam;

    // Another model at the address: no longer a driver.
    strcpy((char*)solid + kSolidName, "TRAFFIC_BODY");
    Check(!GetDriver(solid), "replaced by a non-driver model");
    Cockpit_EndFrame();
    Check(s_retired.empty(), "retired drivers freed at the end of the frame");
}

// A seated driver with a body, an arm and a head: triangles reordered body, arm, head, and the
// entry drawn as the ranges the settings leave in.
static void TestArms() {
    static uint8_t solid[0x100], plat[0x40], entry[kEntrySize], format[16], camera[0x50];
    // head at the back top (x -1, z 1.9..2), body low (z 0), arm in front of the head, beside it
    static float verts[27] = {
        0, 0, 0, 1, 0, 0, 0, 1, 0,                          // body
        -0.6f, 0.5f, 1.6f, -0.4f, 0.5f, 1.6f, -0.5f, 0.5f, 1.7f,   // arm (y 0.5 vs. head 0.33)
        -1, 0, 1.9f, -1, 1, 1.9f, -0.9f, 0, 2.0f};          // head
    static uint16_t indices[9];
    static FakeIb ib;
    const uint16_t original[9] = {6, 7, 8, 3, 4, 5, 0, 1, 2};   // head, arm, body
    memcpy(indices, original, sizeof(indices));
    memcpy(ib.data, original, sizeof(indices));
    ib.size = sizeof(indices);
    strcpy((char*)solid + kSolidName, "ARMCAR_KIT00_DRIVER_A");
    At<float>(solid, kSolidMin) = -1;
    At<float>(solid, kSolidMax + 8) = 2.0f;
    At<uint8_t*>(solid, kSolidPlat) = plat;
    At<int>(plat, kPlatNumEntries) = 1;
    At<uint8_t*>(plat, kPlatEntries) = entry;
    At<uint16_t*>(plat, kPlatIndices) = indices;
    At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) = &ib;
    At<int>(plat, kPlatNumIndices) = 9;
    At<int>(format, 8) = 12;
    At<uint8_t*>(entry, kEntryFormat) = format;
    At<int>(entry, kEntryTris) = 3;
    At<int>(entry, kEntryFirstIndex) = 0;
    At<float*>(entry, kEntryVertices) = verts;
    At<int>(entry, kEntryVertexBytes) = sizeof(verts);

    Driver* d = GetDriver(solid);
    Check(d && d->ok && d->body[0] == 1 && d->arms[0] == 1 && d->heads[0] == 1, "body, arm and head found");
    Check(indices[0] == 0 && indices[3] == 3 && indices[6] == 6 && ib.data[6] == 6, "reordered body, arm, head");

    float model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    uint8_t modelData[0x10] = {};
    At<uint8_t*>(modelData, kModelSolid) = solid;
    memcpy(camera + kCamPosition, d->head, sizeof(d->head));
    void* savedCam = At<void*>((void*)kPlayerView, kViewCamera);
    At<void*>((void*)kPlayerView, kViewCamera) = camera;
    auto draw = [&](bool head, bool arms) {
        g_cfg.hideHead = head;
        g_cfg.hideArms = arms ? 1.0f : 0.0f;
        return OnSubmit(modelData, model, entry);
    };
    uint8_t* e = draw(true, true);
    Check(e && s_driverParts == 1 && At<int>(e, kEntryFirstIndex) == 0 && At<int>(e, kEntryTris) == 1, "body only");
    e = draw(true, false);
    Check(e && s_driverParts == 1 && At<int>(e, kEntryTris) == 2, "body and arms");
    e = draw(false, true);
    Check(e && s_driverParts == 2 && At<int>(e, kEntryTris) == 1 && At<int>(e + kEntrySize, kEntryFirstIndex) == 6 &&
              At<int>(e + kEntrySize, kEntryTris) == 1,
          "body and head, arms left out");
    Check(draw(false, false) == entry, "everything: the entry as it is");
    // Away from the eyes (another car's driver): drawn whole.
    At<float>(camera, kCamPosition) += 5;
    Check(draw(true, true) == entry, "other drivers keep their arms");
    g_cfg.hideHead = false;
    g_cfg.hideArms = 1.0f;
    At<void*>((void*)kPlayerView, kViewCamera) = savedCam;
}

// An interior with a steering wheel (a tilted tube ring) and a dashboard: the ring is found, its
// triangles move to the end of the entry, and a turn to the right lifts the wheel's left side.
static void TestSteeringWheel() {
    const HeadPos* head = CarHead("TESTCAR_KIT00_INTERIOR_A");
    Check(head != nullptr, "driver head known for the car");
    const float tilt = 15.0f * 3.14159265f / 180.0f, radius = 0.16f;
    const float n[3] = {cosf(tilt), 0, -sinf(tilt)}, up[3] = {sinf(tilt), 0, cosf(tilt)};
    const float centre[3] = {head->v[0] + 0.45f, head->v[1], head->v[2] - 0.25f};
    static float verts[4 * 24 + 4][3];
    static uint16_t indices[4096];
    int nv = 0, ni = 0;
    for (int layer = 0; layer < 4; layer++) {       // inner/outer rim, front/back face
        float r = radius + (layer == 1 || layer == 2 ? 0.01f : -0.01f), along = layer < 2 ? -0.01f : 0.01f;
        for (int k = 0; k < 24; k++, nv++) {
            float a = k * 2 * 3.14159265f / 24, left = r * cosf(a), high = r * sinf(a);
            for (int c = 0; c < 3; c++) verts[nv][c] = centre[c] + n[c] * along + up[c] * high;
            verts[nv][1] = centre[1] + left;
        }
    }
    for (int layer = 0; layer < 4; layer++)
        for (int k = 0; k < 24; k++) {
            int a = layer * 24 + k, b = layer * 24 + (k + 1) % 24;
            int c = ((layer + 1) % 4) * 24 + k, d = ((layer + 1) % 4) * 24 + (k + 1) % 24;
            uint16_t quad[6] = {(uint16_t)a, (uint16_t)b, (uint16_t)c, (uint16_t)b, (uint16_t)d, (uint16_t)c};
            for (uint16_t i : quad) indices[ni++] = i;
        }
    int dash = nv;                                  // dashboard, well in front of the wheel
    const float dashQuad[4][3] = {{0.95f, -0.3f, -0.1f}, {0.95f, 0.3f, -0.1f}, {0.95f, -0.3f, 0.1f}, {0.95f, 0.3f, 0.1f}};
    for (int k = 0; k < 4; k++, nv++)
        for (int c = 0; c < 3; c++) verts[nv][c] = head->v[c] + dashQuad[k][c];
    // Dashboard first in the index order, so the ring has to move behind it.
    for (int i = ni - 1; i >= 0; i--) indices[i + 6] = indices[i];
    uint16_t dashTris[6] = {(uint16_t)dash, (uint16_t)(dash + 1), (uint16_t)(dash + 2), (uint16_t)(dash + 1),
                            (uint16_t)(dash + 3), (uint16_t)(dash + 2)};
    memcpy(indices, dashTris, sizeof(dashTris));
    ni += 6;

    static uint8_t solid[0x100], plat[0x40], entry[kEntrySize], format[16];
    static FakeIb ib;
    memcpy(ib.data, indices, ni * 2);
    ib.size = ni * 2;
    strcpy((char*)solid + kSolidName, "TESTCAR_KIT00_INTERIOR_A");
    At<uint8_t*>(solid, kSolidPlat) = plat;
    At<int>(plat, kPlatNumEntries) = 1;
    At<uint8_t*>(plat, kPlatEntries) = entry;
    At<uint16_t*>(plat, kPlatIndices) = indices;
    At<IDirect3DIndexBuffer9*>(plat, kPlatIndexBuffer) = &ib;
    At<int>(plat, kPlatNumIndices) = ni;
    At<int>(format, 8) = 12;
    At<uint8_t*>(entry, kEntryFormat) = format;
    At<int>(entry, kEntryTris) = ni / 3;
    At<int>(entry, kEntryFirstIndex) = 0;
    At<float*>(entry, kEntryVertices) = &verts[0][0];
    At<int>(entry, kEntryVertexBytes) = nv * 12;

    g_cfg.steeringWheel = true;
    SolidInfo* info = GetSolid(solid);
    Wheel* w = info ? info->wheel : nullptr;
    Check(w && w->ok, "steering wheel found in the interior");
    Check(w->turned[0] == 192 && w->kept[0] == 2, "ring triangles selected, dashboard kept");
    Check(indices[0] == dash && ib.data[0] == dash, "wheel triangles moved behind the dashboard");
    for (int c = 0; c < 3; c++) Check(fabsf(w->centre[c] - centre[c]) < 0.005f, "wheel centre");
    Check(fabsf(w->axis[0] - n[0]) < 0.005f && fabsf(w->axis[2] - n[2]) < 0.005f, "steering column tilt");

    // Turning right by 90 degrees: the top of the rim goes to the driver's right (-y).
    float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, turned[16];
    TurnedMatrix(*w, 90.0f, identity, turned);
    float top[3] = {w->centre[0] + up[0] * radius, w->centre[1], w->centre[2] + up[2] * radius};
    float out[3];
    for (int c = 0; c < 3; c++) out[c] = top[0] * turned[c] + top[1] * turned[4 + c] + top[2] * turned[8 + c] + turned[12 + c];
    Check(out[1] < w->centre[1] - radius * 0.9f, "turning right moves the top of the wheel right");
    Near(out[0] * n[0] + out[2] * n[2], w->centre[0] * n[0] + w->centre[2] * n[2], "wheel stays in its plane");
    g_cfg.steeringWheel = false;
}

// Side mirror glass: the big rear-facing flat patch of a mirror model, its size and which way it
// faces; a rear-facing panel that isn't a mirror (behind the cabin, too small) is not taken.
static void AddQuad(std::vector<V3>& t, V3 c, V3 u, V3 v, float hu, float hv, bool flipWinding) {
    V3 p[4] = {{c.x - u.x * hu + v.x * hv, c.y - u.y * hu + v.y * hv, c.z - u.z * hu + v.z * hv},
               {c.x + u.x * hu + v.x * hv, c.y + u.y * hu + v.y * hv, c.z + u.z * hu + v.z * hv},
               {c.x + u.x * hu - v.x * hv, c.y + u.y * hu - v.y * hv, c.z + u.z * hu - v.z * hv},
               {c.x - u.x * hu - v.x * hv, c.y - u.y * hu - v.y * hv, c.z - u.z * hu - v.z * hv}};
    int order[2][6] = {{0, 1, 2, 0, 2, 3}, {0, 2, 1, 0, 3, 2}};
    for (int k : order[flipWinding]) t.push_back(p[k]);
}

static void TestMirrorGlass() {
    // A left mirror: glass facing back and a little out (as in the game's models), a housing
    // around it facing forward and sideways.
    V3 n = mirror::Norm({-0.95f, 0.31f, 0.04f});
    V3 u = mirror::Norm(mirror::Cross({0, 0, 1}, n)), v = mirror::Cross(n, u);
    std::vector<V3> tris;
    AddQuad(tris, {0.5f, 0.9f, 0.8f}, u, v, 0.1f, 0.06f, true);
    AddQuad(tris, {0.55f, 0.9f, 0.8f}, {0, -1, 0}, {0, 0, 1}, 0.11f, 0.07f, false);   // housing front (+x)
    AddQuad(tris, {0.52f, 1.0f, 0.8f}, {1, 0, 0}, {0, 0, 1}, 0.03f, 0.07f, false);    // housing side
    MirrorGlass g = mirror::FindGlass(tris);
    Check(g.ok, "mirror glass 1");
    Check(fabsf(g.centre.x - 0.5f) < 0.01f && fabsf(g.centre.y - 0.9f) < 0.01f && fabsf(g.centre.z - 0.8f) < 0.01f, "mirror glass 2");
    Check(fabsf(g.halfU - 0.1f) < 0.005f && fabsf(g.halfV - 0.06f) < 0.005f, "mirror glass 3");
    Check(g.normal.x < -0.9f, "mirror glass 4");                          // facing the driver, whichever winding
    Check(g.u.y < -0.2f && g.v.z > 0.9f, "mirror glass 5");               // u: to the driver's right (the car's -y), v: up
    // Not a mirror: a rear-facing panel behind the cabin, and a tiny one.
    std::vector<V3> junk;
    AddQuad(junk, {-1.8f, 0.72f, 0.55f}, u, v, 0.05f, 0.05f, false);
    Check(!mirror::FindGlass(junk).ok, "mirror glass 6");
    std::vector<V3> tiny;
    AddQuad(tiny, {0.5f, 0.9f, 0.8f}, u, v, 0.01f, 0.01f, false);
    Check(!mirror::FindGlass(tiny).ok, "mirror glass 7");
    // Rear view mirror: a wide thin sun visor strip (bigger, but not mirror-shaped) is passed over
    // for the mirror itself; it must sit in the region ahead of the head.
    {
        std::vector<V3> inside;
        V3 back = {-1, 0, 0}, ru = mirror::Norm(mirror::Cross({0, 0, 1}, back)), rv = mirror::Cross(back, ru);
        AddQuad(inside, {0.3f, 0.0f, 1.05f}, ru, rv, 0.21f, 0.02f, false);           // visor strip, 0.42 x 0.04
        AddQuad(inside, {0.25f, 0.0f, 1.0f}, ru, rv, 0.1f, 0.032f, true);            // mirror, 0.2 x 0.064
        MirrorGlass r = mirror::FindGlass(inside, false, mirror::GlassKind::RearView);
        Check(r.ok && fabsf(r.centre.x - 0.25f) < 0.01f && fabsf(2 * r.halfU - 0.2f) < 0.01f && r.normal.x < -0.9f,
              "rear view mirror found, visor passed over");
        Check(!mirror::FindGlass(inside, false, mirror::GlassKind::Side).ok, "rear view mirror is not a side mirror");
        const float head[3] = {-0.2f, 0.37f, 0.97f};
        V3 at[3] = {{0.25f, 0.05f, 1.0f}, {0.3f, -0.05f, 1.0f}, {0.25f, 0.0f, 1.05f}};
        V3 ahead[3] = {{1.2f, 0.0f, 1.0f}, {1.3f, 0.0f, 1.0f}, {1.2f, 0.0f, 1.05f}};
        Check(mirror::InRearViewRegion(at, head) && !mirror::InRearViewRegion(ahead, head), "rear view region");
    }
    // The body search region: out at the side, at mirror height.
    V3 in[3] = {{0.5f, 0.9f, 0.8f}, {0.6f, 0.9f, 0.8f}, {0.5f, 0.95f, 0.85f}};
    V3 low[3] = {{0.5f, 0.9f, 0.3f}, {0.6f, 0.9f, 0.3f}, {0.5f, 0.95f, 0.35f}};
    // The body search (either winding) on a door-top strip with the glass sticking out of it.
    std::vector<V3> body;
    AddQuad(body, {0.0f, 0.85f, 0.85f}, {1, 0, 0}, {0, 0, 1}, 0.5f, 0.1f, false);      // door top, facing out
    AddQuad(body, {0.5f, 0.9f, 0.8f}, u, v, 0.1f, 0.06f, false);
    MirrorGlass b = mirror::FindGlass(body, false);
    Check(b.ok && fabsf(b.centre.x - 0.5f) < 0.01f && b.normal.x < -0.9f, "mirror glass in the body");
    Check(mirror::InBodyMirrorRegion(in, 1.0f) && !mirror::InBodyMirrorRegion(in, -1.0f) && !mirror::InBodyMirrorRegion(low, 1.0f), "mirror glass 8");
}

// The levelled camera stays upright when the car is on its roof (rolled 180 degrees), and a
// half-levelled one keeps half the roll.
static void TestLevelUpsideDown() {
    const V3 up = {0, 0, 1};
    auto camera = [](V3 r, V3 d, V3 f) {
        M4 w = Identity();
        const V3 axes[3] = {r, d, f};
        for (int i = 0; i < 3; i++) { w.m[i][0] = axes[i].x; w.m[i][1] = axes[i].y; w.m[i][2] = axes[i].z; }
        w.m[3][0] = 5; w.m[3][1] = 6; w.m[3][2] = 7;
        return MInvRigid(w);
    };
    M4 upright = MInvRigid(LevelCamera(camera({1, 0, 0}, {0, 0, -1}, {0, 1, 0}), up, 1, 0));
    Near(upright.m[0][0], 1, "upright camera keeps its right axis");
    Near(upright.m[1][2], -1, "upright camera keeps down");
    M4 roof = MInvRigid(LevelCamera(camera({-1, 0, 0}, {0, 0, 1}, {0, 1, 0}), up, 1, 0));
    Near(roof.m[0][0], 1, "car on its roof: right axis levelled back");
    Near(roof.m[1][2], -1, "car on its roof: down still points at the ground");
    Near(roof.m[2][1], 1, "car on its roof: same heading");
    Near(roof.m[3][2], 7, "levelling keeps the position");
    const float s = 0.70710678f;
    M4 half = MInvRigid(LevelCamera(camera({s, 0, -s}, {-s, 0, -s}, {0, 1, 0}), up, 0.5f, 0));
    Near(half.m[0][0], cosf(3.14159265f / 8), "half levelling keeps half the roll");
}

// The dashboard panel: the first surface past the wheel along the line from the head over its top.
static void TestDash() {
    Wheel w;
    w.centre[0] = 0.4f; w.centre[1] = 0.4f; w.centre[2] = 0.7f;
    w.axis[0] = 0.94f; w.axis[1] = 0; w.axis[2] = -0.34f;
    w.radius = 0.16f;
    const float head[3] = {-0.1f, 0.4f, 0.95f};
    // A cluster face at x = 0.62 (behind the wheel, draw entry 1, plus a small gauge triangle near the
    // hit), a wall in front of the wheel at x = 0.3 and a far-off triangle of the same entry.
    auto tri = [](V3 a, V3 b, V3 c, int entry, uint16_t v) {
        TriRef t;
        t.p[0] = a; t.p[1] = b; t.p[2] = c;
        t.v[0] = v; t.v[1] = (uint16_t)(v + 1); t.v[2] = (uint16_t)(v + 2);
        t.entry = entry;
        t.id = TriId(t.v[0], t.v[1], t.v[2]);
        return t;
    };
    std::vector<TriRef> tris = {
        tri({0.62f, 0.0f, 0.4f}, {0.62f, 0.8f, 0.4f}, {0.62f, 0.4f, 1.2f}, 1, 0),
        tri({0.3f, 0.0f, 0.0f}, {0.3f, 0.8f, 0.0f}, {0.3f, 0.4f, 0.2f}, 0, 3),
        tri({0.62f, 0.35f, 0.8f}, {0.62f, 0.45f, 0.8f}, {0.62f, 0.4f, 0.85f}, 1, 6),
        tri({0.62f, 1.5f, 0.0f}, {0.62f, 1.6f, 0.0f}, {0.62f, 1.55f, 0.1f}, 1, 9),
    };
    FindDash(w, tris, head, "TEST_INTERIOR");
    Check(w.dashOk, "dashboard placed");
    Check(w.dash.x > 0.58f && w.dash.x < 0.6f, "dashboard plane 3 cm off the cluster face, toward the driver");
    Check(w.dashN.x < -0.8f, "dashboard faces the driver");
    Check(w.dashUp.z > 0.9f, "dashboard upright");
    Check(w.dashRight.y < -0.9f, "dashboard right is the driver's right");
    bool nearOne = false, farOne = false;
    for (uint64_t id : w.gauges) {
        nearOne |= id == tris[2].id;
        farOne |= id == tris[3].id;
    }
    Check(nearOne && !farOne, "gauge triangles: near the hit in its draw entry, not far off");
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 20, 30, 1};
    float o[3], r[3], u[3], t[3];
    g_cfg.dashRight = 0.1f; g_cfg.dashUp = 0;
    DashBasis(w, m, o, r, u, t);
    Check(fabsf(o[0] - 10 - w.dash.x) < 0.02f && fabsf(o[1] - 20 - (w.dash.y - 0.1f)) < 0.001f,
          "plane origin in the world, moved right (the car's -y)");
    Near(r[1], w.dashRight.y, "plane right in the world");
    g_cfg.dashRight = 0;
}

// Triangle ids ignore order and winding; the edits file's text round-trips.
static void TestEditsText() {
    Check(TriId(5, 2, 9) == TriId(9, 5, 2) && TriId(2, 9, 5) == TriId(2, 5, 9), "triangle id ignores order");
    Check(TriId(1, 2, 3) != TriId(1, 2, 4), "triangle ids differ");
    auto t = ParseTris("1.2.3, 40.50.60,7.8.9");
    Check(t.size() == 3 && t[1][0] == 40 && t[1][2] == 60, "edits text parsed");
    Check(TrisText(t) == "1.2.3,40.50.60,7.8.9", "edits text written back");
    Check(IdTri(TriId(9, 4, 7))[0] == 4 && IdTri(TriId(9, 4, 7))[2] == 9, "id back to its vertices");
}

int main() {
    TestDash();
    TestEditsText();
    TestLevelUpsideDown();
    TestMirrorGlass();
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
    // Seated, the game's "camera inside the car" test never hides the car (looking down, IS300).
    {
        float mn[3] = {-2, -1, 0}, mx[3] = {2, 1, 1.4f}, eye[3] = {0, 0.4f, 1.2f};
        Check(s_seatedConn != nullptr, "seat applied this frame");
        Check(hkCarCameraInside(mn, mx, eye, 0) == 0, "seated eyes inside the car keep it drawn");
    }

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
    TestReloadedDriver();
    TestArms();
    TestSteeringWheel();
    puts("PASS: cockpit motion, offsets, current POV, unload, anchor switch, LOD, frame lifecycle, reloaded drivers and steering wheel");
}
