// Speedbreaker meter -> car damage meter with an engine heat overlay.
//
// LocalPlayer feeds the speedbreaker charge into the HUD through ISpeedBreakerMeter::
// SetPursuitLevel(0..1) every HUD update, and SpeedBreakerMeter::Update turns it into the arc
// right of the tachometer: a masked FEMultiImage ("Speedbreaker_Meter_Multi_Image") whose mask is
// rotated to 175 * (1 - level) degrees. That slot is replaced, so the arc shows the car's damage
// instead, all in the game's own art and position (and on the VR mod's HUD panel).
//
// The game's arc fills from the top down; with [HUD] FillUp the mask is turned the other way
// (after SpeedBreakerMeter::Update, which sets it only when the level changes) so it fills
// from the bottom up.
//
// The heat overlay is a clone of that arc (FEObject::Clone), inserted right after it in its parent
// group so it draws on top, tinted red and rotated by the heat above HeatFrom. The clock icon
// (SPEEDBREAKER_METER_ICON_GROUP) gets the pause menu's car icon. While heat or damage is at
// Warn or above both arcs pulse. The car icon is drawn like the game's own HUD: gray with no
// damage, the HUD's beige (the arc's colour) when damaged, and pulsing red like the arc from
// IconRedFrom (75%) up. Above it a clone of it shows the spike strip milestone icon
// (MILESTONE_SPIKESTRIPS) the same way: gray, flashing red while a tire is popped. It stays
// opaque (it sits over the arc, so a translucent one takes on the arc's colour): it flashes by
// brightness, in the car icon's rhythm. Neither icon has an outline. The HUD package owns the clones and frees them with the
// rest of the package; a new HUD (or a clone that is gone from its list) gets new ones.
#include "hud.h"
#include "common.h"
#include "game.h"
#include <math.h>

namespace {

// --- speed.exe v1.3 -------------------------------------------------------------------------
constexpr uintptr_t kSlotSetPursuitLevel = 0x8A2110;// SpeedBreakerMeter's ISpeedBreakerMeter vtable +4
constexpr uintptr_t kSetPursuitLevel = 0x5685C0;    // void SpeedBreakerMeter::SetPursuitLevel(float)
constexpr uintptr_t kSlotUpdate = 0x8A2118;         // SpeedBreakerMeter's HudElement vtable +4
constexpr uintptr_t kUpdate = 0x568460;             // void SpeedBreakerMeter::Update(IPlayer*)
constexpr uintptr_t kFEngInstance = 0x91CADC;       // cFEng* cFEng::mInstance
constexpr uintptr_t kFindPackage = 0x516AF0;        // FEPackage* cFEng::FindPackage(const char*)
constexpr uintptr_t kAddNode = 0x5AEE90;            // void FEMinList::AddNode(FEMinNode* after, FEMinNode* node)
constexpr uintptr_t kSetColor = 0x5157E0;           // void FEngSetColor(FEObject*, unsigned argb)
constexpr uintptr_t kGetColor = 0x51A180;           // unsigned FEngGetColor(FEObject*)
constexpr uintptr_t kSetTextureHash = 0x515B90;     // void FEngSetTextureHash(FEImage*, unsigned)
constexpr uintptr_t kStringHash = 0x460BF0;         // unsigned bStringHash(const char*)
constexpr uintptr_t kGetTextureInfo = 0x503400;     // TextureInfo* GetTextureInfo(unsigned, int, int)
constexpr uintptr_t kVtblMultiImage = 0x8A2E3C;
constexpr uintptr_t kIListFind = 0x5D59F0;          // IUnknown* UTL::COM::Object::_IList::Find(void*)
constexpr uintptr_t kIGenericMessageHandle = 0x5650B0;
constexpr uintptr_t kRequestGenericMessage = 0x568030; // bool GenericMessage::RequestGenericMessage(text, singleFrame,
                                                    //   fengHash, iconTexture, iconFengHash, priority)

// SpeedBreakerMeter (HudElement, ISpeedBreakerMeter at +0x30)
constexpr int kSB_Iface = 0x30;
constexpr int kSB_Package = 0x10;                   // const char* package name
constexpr int kSB_Icon = 0x40;                      // SPEEDBREAKER_METER_ICON_GROUP
constexpr int kSB_Bar = 0x44;                       // Speedbreaker_Meter_Multi_Image

// FEObject / FEGroup / FEPackage
constexpr int kFO_Next = 0x04;
constexpr int kFO_NameHash = 0x10;
constexpr int kFO_Type = 0x18;
constexpr int kFO_Flags = 0x1C;
constexpr int kFO_Handle = 0x24;                    // images: texture hash
constexpr int kFO_Data = 0x2C;                      // FEObjData*: Col, Pivot, Pos, Rot, Size, ...
constexpr int kFO_DataSize = 0x30;
constexpr int kFO_Cached = 0x58;                    // render object, made on demand (Clone leaves it unset)
constexpr int kMI_Textures = 0x60;                  // FEMultiImage: hTexture[3], TextureFlags[3] (Clone skips them)
constexpr int kFG_Children = 0x5C;                  // FEGroup: FEMinList (count +4, head +8, tail +0xC)
constexpr int kPK_Objects = 0x44;                   // FEPackage: FEMinList of top-level objects
constexpr int kML_Head = 0x08;
constexpr int kOD_SizeX = 0x38;                     // FEObjData: Size.x, Size.y
constexpr int kOD_SizeY = 0x3C;
constexpr int kOD_PosX = 0x1C;                      // FEObjData: Pos.x
constexpr int kOD_PosY = 0x20;                      // FEObjData: Pos.y (screen y, down)
constexpr int kOD_PosZ = 0x24;                      // FEObjData: Pos.z (objects are drawn far to near)
constexpr int kMID_Rot = 0x8C;                      // FEMultiImageData: mask rotation (degrees)
constexpr uint32_t kDirty = 0x03C00000;             // FF_Dirty | FF_DirtyTransform | FF_DirtyColor | FF_DirtyCode
enum { FE_Image = 1, FE_Group = 5, FE_MultiImage = 12 };

constexpr uint32_t kHeatColor = 0xFFE01E14;
constexpr uint32_t kIconGray = 0xFFA8A8A8;          // car icon with no damage
constexpr float kIconRedFrom = 0.75f;               // car icon pulses red from this much damage
constexpr float kIconDamaged = 0.005f;              // any damage at all turns it beige
constexpr uint32_t kOutlineColor = 0xC0000000;
constexpr float kOutline = 1.5f;                    // outline width, in HUD units
constexpr float kOutlineDepth = 0.01f;              // outline copies just behind their icon
constexpr float kMinLevel = 0.001f;                 // lowest level the game's meter is given
constexpr float kSpikeGap = 0.25f;                  // space between the car and spike icons, in icon heights

typedef void(__thiscall* SetLevel_t)(void* iface, float level);
typedef void(__thiscall* MeterUpdate_t)(void* meter, void* player);
typedef void*(__thiscall* IListFind_t)(void* list, void* handle);
typedef bool(__thiscall* RequestMessage_t)(void* gm, const char* text, bool singleFrame, uint32_t fengHash,
                                           uint32_t iconTexture, uint32_t iconFengHash, int priority);
typedef uint8_t*(__thiscall* FindPackage_t)(void* feng, const char* name);
typedef uint8_t*(__thiscall* Clone_t)(void* obj, bool reference);
typedef void(__thiscall* AddNode_t)(void* list, void* after, void* node);
typedef void(__cdecl* SetColor_t)(void* obj, uint32_t argb);
typedef uint32_t(__cdecl* GetColor_t)(void* obj);
typedef void(__cdecl* SetTextureHash_t)(void* img, uint32_t hash);
typedef uint32_t(__cdecl* StringHash_t)(const char*);
typedef void*(__cdecl* GetTextureInfo_t)(uint32_t hash, int returnDefault, int includeUnloaded);

template <class T> T& Field(uint8_t* p, int off) { return *(T*)(p + off); }
void** Vtbl(void* obj) { return *(void***)obj; }

uint8_t* ListHead(uint8_t* list) { return Field<uint8_t*>(list, kML_Head); }
uint8_t* Next(uint8_t* obj) { return Field<uint8_t*>(obj, kFO_Next); }

// The FEMinList (package objects or a group's children) that holds obj.
uint8_t* FindList(uint8_t* list, uint8_t* obj, int depth = 0) {
    if (depth > 16) return nullptr;
    for (uint8_t* o = ListHead(list); o; o = Next(o)) {
        if (o == obj) return list;
        if (Field<int>(o, kFO_Type) == FE_Group) {
            uint8_t* found = FindList(o + kFG_Children, obj, depth + 1);
            if (found) return found;
        }
    }
    return nullptr;
}

bool InList(uint8_t* list, uint8_t* obj) {
    for (uint8_t* o = ListHead(list); o; o = Next(o))
        if (o == obj) return true;
    return false;
}

void SetColor(uint8_t* obj, uint32_t argb) { ((SetColor_t)kSetColor)(obj, argb); }
uint32_t GetColor(uint8_t* obj) { return ((GetColor_t)kGetColor)(obj); }
uint32_t WithAlpha(uint32_t argb, float a) {
    uint32_t alpha = (uint32_t)Clamp((float)(argb >> 24) * a, 0.0f, 255.0f);
    return (argb & 0x00FFFFFF) | (alpha << 24);
}

// Opaque colour at brightness k (0..1).
uint32_t Bright(uint32_t argb, float k) {
    uint32_t out = 0xFF000000;
    for (int sh = 0; sh < 24; sh += 8) out |= (uint32_t)Clamp(((argb >> sh) & 0xFF) * k, 0.0f, 255.0f) << sh;
    return out;
}

void SetRotation(uint8_t* multiImage, float degrees) {
    uint8_t* data = Field<uint8_t*>(multiImage, kFO_Data);
    if (!data) return;
    Field<float>(data, kMID_Rot) = degrees;
    Field<uint32_t>(multiImage, kFO_Flags) |= 0x400000;
}

void DumpGroup(const char* what, uint8_t* list, int depth) {
    for (uint8_t* o = ListHead(list); o && depth < 4; o = Next(o)) {
        Log("hud: %s %*s%p type %d hash %08X tex %08X color %08X", what, depth * 2, "", o, Field<int>(o, kFO_Type),
            Field<uint32_t>(o, kFO_NameHash), Field<uint32_t>(o, kFO_Handle), GetColor(o));
        if (Field<int>(o, kFO_Type) == FE_Group) DumpGroup(what, o + kFG_Children, depth + 1);
    }
}

// --- state (game thread only) -------------------------------------------------------------
struct IconImage {
    uint8_t* img;
    uint32_t tex;           // original texture
    uint32_t color;         // original colour
};

struct {
    uint8_t* meter = nullptr;
    uint8_t* bar = nullptr;
    uint8_t* list = nullptr;        // the list holding bar and clone
    uint8_t* clone = nullptr;
    uint32_t barColor = 0;          // bar's own colour, restored after flashing
    std::vector<IconImage> icon;
    uint8_t* iconList = nullptr;    // the list holding the car icon image and the spike icon
    uint8_t* spike = nullptr;       // spike strip icon (a clone of the car icon)
    std::vector<uint8_t*> carOutline, spikeOutline;
    uint32_t spikeTex = 0;          // texture hash, 0 = not loaded
    bool spikeLooked = false;
    bool applied = false;           // our look is on (icon swapped, clone present)
    bool failed = false;            // couldn't set up for this meter, don't retry every frame
    uint32_t carIcon = 0;           // texture hash, 0 = none found
    double seen = -1e9;             // last SetPursuitLevel (the meter and its HUD exist)
    float minAngle = 175.0f;        // mask rotation of an empty arc (the game's own fill direction)
    HudStatus h;                    // last status shown
} s;

// Mask rotation for a level 0..1. The game turns the mask from minAngle (empty) to 0 (full);
// filling up turns it from the opposite side, minAngle - 180 (full) to -180 (empty).
float Angle(float level) {
    level = Clamp(level, 0.0f, 1.0f);
    if (!g_cfg.hudFillUp) return s.minAngle * (1.0f - level);
    float flip = s.minAngle > 0.0f ? -180.0f : 180.0f;
    return flip + s.minAngle * level;
}

uint32_t PickIcon() {
    const char* names[] = {g_cfg.hudIcon.c_str(), "INGAME_ICON_PLAYERCAR", "COPS_ICON", "HIDE_CAR_ICON"};
    for (const char* n : names) {
        if (!*n) continue;
        uint32_t h = ((StringHash_t)kStringHash)(n);
        if (((GetTextureInfo_t)kGetTextureInfo)(h, 0, 0)) {
            Log("hud: icon texture %s (%08X)", n, h);
            return h;
        }
        Log("hud: icon texture %s not loaded", n);
    }
    return 0;
}

void CollectIcon(uint8_t* list, int depth) {
    for (uint8_t* o = ListHead(list); o && depth < 4; o = Next(o)) {
        int type = Field<int>(o, kFO_Type);
        if (type == FE_Image) s.icon.push_back({o, Field<uint32_t>(o, kFO_Handle), GetColor(o)});
        else if (type == FE_Group) CollectIcon(o + kFG_Children, depth + 1);
    }
}

void Setup(uint8_t* meter) {
    s.meter = meter;
    s.bar = Field<uint8_t*>(meter, kSB_Bar);
    s.list = s.clone = s.iconList = s.spike = nullptr;
    s.icon.clear();
    s.carOutline.clear();
    s.spikeOutline.clear();
    s.applied = s.failed = false;
    const char* pkgName = Field<const char*>(meter, kSB_Package);
    uint8_t* feng = *(uint8_t**)kFEngInstance;
    uint8_t* pkg = feng && pkgName ? ((FindPackage_t)kFindPackage)(feng, pkgName) : nullptr;
    if (!s.bar || !pkg || *(uintptr_t*)s.bar != kVtblMultiImage) {
        Log("hud: meter %p in %s has no arc (bar %p, package %p)", meter, pkgName ? pkgName : "?", s.bar, pkg);
        s.failed = true;
        return;
    }
    s.list = FindList(pkg + kPK_Objects, s.bar);
    if (!s.list) {
        Log("hud: arc %p not found in %s", s.bar, pkgName);
        s.failed = true;
        return;
    }
    s.barColor = GetColor(s.bar);
    Log("hud: meter %p in %s: arc %p colour %08X tex %08X", meter, pkgName, s.bar, s.barColor,
        Field<uint32_t>(s.bar, kMI_Textures));
    uint8_t* icon = Field<uint8_t*>(meter, kSB_Icon);
    if (icon && Field<int>(icon, kFO_Type) == FE_Group) {
        DumpGroup("icon", icon + kFG_Children, 0);
        CollectIcon(icon + kFG_Children, 0);
        // The group also holds a small image at the end of the bar; only the clock (the largest
        // image) becomes the car.
        size_t big = 0;
        float bigArea = -1.0f;
        for (size_t i = 0; i < s.icon.size(); i++) {
            uint8_t* d = Field<uint8_t*>(s.icon[i].img, kFO_Data);
            float area = d ? fabsf(Field<float>(d, kOD_SizeX) * Field<float>(d, kOD_SizeY)) : 0.0f;
            Log("hud: icon image %p size %.1f", s.icon[i].img, area);
            if (area > bigArea) bigArea = area, big = i;
        }
        if (!s.icon.empty()) s.icon = {s.icon[big]};
        if (!s.icon.empty()) s.iconList = FindList(icon + kFG_Children, s.icon[0].img);
    }
    if (!s.carIcon) s.carIcon = PickIcon();
    if (!s.spikeLooked) {
        s.spikeLooked = true;
        uint32_t h = ((StringHash_t)kStringHash)("MILESTONE_SPIKESTRIPS");
        s.spikeTex = ((GetTextureInfo_t)kGetTextureInfo)(h, 0, 0) ? h : 0;
        Log("hud: spike strip icon %s", s.spikeTex ? "loaded" : "not loaded");
    }
}

bool MakeClone() {
    uint8_t* c = ((Clone_t)Vtbl(s.bar)[1])(s.bar, false);
    if (!c) return false;
    Field<void*>(c, kFO_Cached) = nullptr;
    uint8_t* src = Field<uint8_t*>(s.bar, kFO_Data);
    uint8_t* dst = Field<uint8_t*>(c, kFO_Data);
    if (src && dst) memcpy(dst, src, std::min(Field<uint32_t>(c, kFO_DataSize), Field<uint32_t>(s.bar, kFO_DataSize)));
    memcpy(c + kMI_Textures, s.bar + kMI_Textures, 0x18);
    Field<uint32_t>(c, kFO_Flags) |= kDirty;
    ((AddNode_t)kAddNode)(s.list, s.bar, c);
    s.clone = c;
    Log("hud: heat overlay %p added", c);
    return true;
}

// A copy of the car icon image, moved by (dx, dy) and dz further back, with texture tex.
uint8_t* CloneIcon(float dx, float dy, float dz, uint32_t tex) {
    uint8_t* img = s.icon[0].img;
    uint8_t* c = ((Clone_t)Vtbl(img)[1])(img, false);
    if (!c) return nullptr;
    Field<void*>(c, kFO_Cached) = nullptr;
    uint8_t* src = Field<uint8_t*>(img, kFO_Data);
    uint8_t* dst = Field<uint8_t*>(c, kFO_Data);
    if (!src || !dst) return nullptr;
    memcpy(dst, src, std::min(Field<uint32_t>(c, kFO_DataSize), Field<uint32_t>(img, kFO_DataSize)));
    Field<float>(dst, kOD_PosX) += dx;
    Field<float>(dst, kOD_PosY) += dy;
    Field<float>(dst, kOD_PosZ) += dz;
    ((SetTextureHash_t)kSetTextureHash)(c, tex);
    Field<uint32_t>(c, kFO_Flags) |= kDirty;
    ((AddNode_t)kAddNode)(s.iconList, img, c);
    return c;
}

// The spike strip icon sits above the car icon by its height plus a gap.
float SpikeOffset() {
    uint8_t* d = Field<uint8_t*>(s.icon[0].img, kFO_Data);
    return d ? -fabsf(Field<float>(d, kOD_SizeY)) * (1.0f + kSpikeGap) : 0.0f;
}

// Outline copies of an icon (at dy above the car icon) in the eight directions around it; shown
// while show and the car icon are. Returns false if they can't be made.
bool UpdateOutline(std::vector<uint8_t*>& out, float dy, uint32_t tex, bool show) {
    if (out.empty() || !InList(s.iconList, out[0])) {
        out.clear();
        for (int i = 0; i < 8; i++) {
            float a = i * 0.78539816f;
            uint8_t* c = CloneIcon(cosf(a) * kOutline, dy + sinf(a) * kOutline, kOutlineDepth, tex);
            if (!c) return false;
            out.push_back(c);
        }
        Log("hud: outline %p.. added", out[0]);
    }
    uint32_t hide = (Field<uint32_t>(s.icon[0].img, kFO_Flags) & 1u) | (show ? 0u : 1u);
    for (uint8_t* c : out) {
        SetColor(c, kOutlineColor);
        uint32_t& flags = Field<uint32_t>(c, kFO_Flags);
        if ((flags & 1u) != hide) flags = (flags & ~1u) | hide | kDirty;
    }
    return true;
}

bool MakeSpike() {
    uint8_t* c = CloneIcon(0.0f, SpikeOffset(), 0.0f, s.spikeTex);
    if (!c) return false;
    s.spike = c;
    Log("hud: spike strip icon %p added (y offset %.1f)", c, SpikeOffset());
    return true;
}

void UpdateSpike(int popped, float flash) {
    if (!s.spikeTex || !s.iconList || s.icon.empty()) return;
    if (!s.spike || !InList(s.iconList, s.spike)) {
        s.spike = nullptr;
        if (!MakeSpike()) {
            s.spikeTex = 0;
            return;
        }
    }
    SetColor(s.spike, popped ? Bright(kHeatColor, flash) : kIconGray);
    // Bit 0 = invisible (FEngSetInvisible): shown whenever the car icon is.
    uint32_t& flags = Field<uint32_t>(s.spike, kFO_Flags);
    uint32_t vis = (flags & ~1u) | (Field<uint32_t>(s.icon[0].img, kFO_Flags) & 1u);
    if (vis != flags) flags = vis | kDirty;
    if (!s.spikeOutline.empty() && InList(s.iconList, s.spikeOutline[0]))
        UpdateOutline(s.spikeOutline, SpikeOffset(), s.spikeTex, false);
}

void Apply() {
    if (!s.clone || !InList(s.list, s.clone)) {
        s.clone = nullptr;
        if (!MakeClone()) {
            s.failed = true;
            return;
        }
    }
    if (!s.applied && s.carIcon)
        for (auto& i : s.icon) ((SetTextureHash_t)kSetTextureHash)(i.img, s.carIcon);
    s.applied = true;
}

void Restore() {
    if (!s.applied) return;
    for (auto& i : s.icon) {
        ((SetTextureHash_t)kSetTextureHash)(i.img, i.tex);
        SetColor(i.img, i.color);
    }
    if (s.bar) {
        SetColor(s.bar, s.barColor);
        SetRotation(s.bar, s.minAngle * (1.0f - Field<float>(s.meter, kSB_Iface + 8)));   // the game's level
    }
    if (s.clone && InList(s.list, s.clone)) SetColor(s.clone, WithAlpha(kHeatColor, 0.0f));
    if (s.spike && InList(s.iconList, s.spike)) SetColor(s.spike, 0);
    for (auto* out : {&s.carOutline, &s.spikeOutline})
        if (!out->empty() && InList(s.iconList, (*out)[0]))
            for (uint8_t* c : *out) SetColor(c, 0);
    s.applied = false;
}

void Update(uint8_t* meter, const HudStatus& h) {
    if (meter != s.meter || Field<uint8_t*>(meter, kSB_Bar) != s.bar) Setup(meter);
    if (s.failed) return;
    const char* pkgName = Field<const char*>(meter, kSB_Package);
    s.minAngle = pkgName && _stricmp(pkgName, "HUD_Drag.fng") == 0 ? -48.0f : 175.0f;
    Apply();
    if (s.failed) return;

    float heat = Clamp((h.heat - g_cfg.hudHeatFrom) / (1.0f - g_cfg.hudHeatFrom), 0.0f, 1.0f);
    bool warn = h.heat >= g_cfg.hudWarn || h.damage >= g_cfg.hudWarn;
    // Quick fade: 1 -> 0.2 -> 1 every 0.5 s.
    float pulse = warn ? 0.6f + 0.4f * cosf((float)fmod(Now(), 0.5) * 6.2831853f / 0.5f) : 1.0f;
    SetColor(s.bar, WithAlpha(s.barColor, pulse));
    SetRotation(s.bar, Angle(h.damage));
    SetRotation(s.clone, Angle(heat));
    float wave = 0.5f + 0.5f * cosf((float)fmod(Now(), 0.5) * 6.2831853f / 0.5f);
    float flash = 0.2f + 0.8f * wave;               // same swing as the car icon's red pulse
    // The car icon in the HUD's own colours, no outline (an outlined, opaque copy looked harsh).
    uint32_t iconColor = h.damage >= kIconRedFrom ? WithAlpha(kHeatColor, 0.6f + 0.4f * cosf((float)fmod(Now(), 0.5) * 6.2831853f / 0.5f))
                         : h.damage >= kIconDamaged ? (s.barColor | 0xFF000000)
                         : kIconGray;
    for (auto& i : s.icon)
        SetColor(i.img, iconColor);
    if (!s.carOutline.empty() && InList(s.iconList, s.carOutline[0]))
        UpdateOutline(s.carOutline, 0.0f, Field<uint32_t>(s.icon[0].img, kFO_Handle), false);
    UpdateSpike(h.poppedTires, flash);
    // Visible whenever the arc is (bit 0 = invisible, FEngSetInvisible); the clone keeps the
    // flag the arc had when it was cloned and the game never touches it again.
    uint32_t& flags = Field<uint32_t>(s.clone, kFO_Flags);
    uint32_t vis = (flags & ~1u) | (Field<uint32_t>(s.bar, kFO_Flags) & 1u);
    if (vis != flags) flags = vis | kDirty;
    // FEngine sorts objects by depth: keep the overlay just in front of the arc.
    uint8_t* barData = Field<uint8_t*>(s.bar, kFO_Data);
    uint8_t* cloneData = Field<uint8_t*>(s.clone, kFO_Data);
    if (barData && cloneData) Field<float>(cloneData, kOD_PosZ) = Field<float>(barData, kOD_PosZ) + g_cfg.hudDepth;
    SetColor(s.clone, WithAlpha(kHeatColor, heat > 0.0f ? pulse : 0.0f));
    static double lastLog;
    if (g_cfg.logInputs && Now() - lastLog > 2.0) {
        lastLog = Now();
        uint8_t* d = Field<uint8_t*>(s.clone, kFO_Data);
        Log("hud: z %.2f/%.2f damage %.2f heat %.2f warn %d | arc %08X rot %.1f | overlay %08X rot %.1f flags %08X", Field<float>(Field<uint8_t*>(s.bar, kFO_Data), kOD_PosZ), d ? Field<float>(d, kOD_PosZ) : 0.0f, h.damage, h.heat,
            warn, GetColor(s.bar), Field<float>(Field<uint8_t*>(s.bar, kFO_Data), kMID_Rot), GetColor(s.clone),
            d ? Field<float>(d, kMID_Rot) : -1.0f, Field<uint32_t>(s.clone, kFO_Flags));
    }
}

void __fastcall Hook_SetPursuitLevel(uint8_t* iface, void*, float level) {
    if (!g_cfg.hudMeter) {
        Restore();
        ((SetLevel_t)kSetPursuitLevel)(iface, level);
        return;
    }
    s.seen = Now();
    HudStatus h = Game_Hud();
    s.h = h;
    // SpeedBreakerMeter::Update plays one script on the icon group at level 0 and another above
    // it; the level-0 one leaves the group at the arc's depth, where the arc can draw over the
    // icons. Never 0, so the icons stay in front (the arc is turned from h.damage, not this).
    ((SetLevel_t)kSetPursuitLevel)(iface, std::max(h.damage, kMinLevel));
    Update(iface - kSB_Iface, h);
}

// The game's Update turns the arc's mask when the level changes; ours is put back after it.
void __fastcall Hook_MeterUpdate(uint8_t* meter, void*, void* player) {
    ((MeterUpdate_t)kUpdate)(meter, player);
    if (g_cfg.hudMeter && s.applied && meter == s.meter && s.bar && Now() - s.seen < 0.5)
        SetRotation(s.bar, Angle(s.h.damage));
}

}  // namespace

bool Hud_Message(const char* text) {
    if (!s.meter || Now() - s.seen > 0.5) return false;
    void* outer = Field<void*>(s.meter + kSB_Iface, 4);    // IUnknown -> its UTL::COM::Object
    void* gm = outer ? ((IListFind_t)kIListFind)(outer, (void*)kIGenericMessageHandle) : nullptr;
    if (!gm || Vtbl(gm)[1] != (void*)kRequestGenericMessage) {
        Log("hud: no generic message element");
        return false;
    }
    // Style and icon slot of the "cop taken out" message (FePursuitBoard).
    return ((RequestMessage_t)kRequestGenericMessage)(gm, text, false, 0x8AB83EDB, s.carIcon, 0x13FF94, 4);
}

void Hud_Install() {
    if (*(uintptr_t*)kSlotSetPursuitLevel != kSetPursuitLevel || *(uintptr_t*)kSlotUpdate != kUpdate) {
        Log("hud: unexpected SpeedBreakerMeter vtable, meter unchanged");
        return;
    }
    for (auto slot : {std::make_pair(kSlotSetPursuitLevel, (void*)&Hook_SetPursuitLevel),
                      std::make_pair(kSlotUpdate, (void*)&Hook_MeterUpdate)}) {
        DWORD old;
        VirtualProtect((void*)slot.first, 4, PAGE_READWRITE, &old);
        memcpy((void*)slot.first, &slot.second, 4);
        VirtualProtect((void*)slot.first, 4, old, &old);
    }
    Log("hud: speedbreaker meter shows damage and heat (%s)", g_cfg.hudMeter ? "on" : "off");
}
