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
// IconRedFrom (75%) up. (Now: gray under 1% damage, the speedbreaker clock icon's own beige from 1%,
// blinking red like the other two icons from 75%; all three moved [HUD] IconShift right of the
// arc's arrow.) Above it a clone of it shows the spike strip milestone icon
// (MILESTONE_SPIKESTRIPS), only while a tire is popped, flashing red. It stays opaque (it sits
// over the arc, so a translucent one takes on the arc's colour): it flashes by brightness, in
// the car icon's rhythm. Neither icon has an outline. Left of the spike strip icon, at the same
// spacing, an engine warning icon ([HUD] HeatIcon) shows the same way, only while the engine heat
// is at HeatIconFrom (50%) or above or the engine is blown. The HUD package owns the clones and frees them with the
// rest of the package; a new HUD (or a clone that is gone from its list) gets new ones.
#include "hud.h"
#include "common.h"
#include "game.h"
#include "physics.h"
#include "iconpack.h"
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
constexpr uintptr_t kVtblGenericMessage = 0x8A2088;   // its IGenericMessage vtable (+4 RequestGenericMessage)
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
constexpr float kIconDamaged = 0.01f;               // from 1% damage it turns beige
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
    uint8_t* pkg = nullptr;         // the HUD package they are in, and its name (to tell it still exists)
    std::string pkgName;
    uint8_t* clone = nullptr;
    uint32_t barColor = 0;          // bar's own colour, restored after flashing
    std::vector<IconImage> icon;
    IconImage arrow = {};           // the small arrow at the arc's end (in the icon group), coloured like the car icon
    uint8_t* iconList = nullptr;    // the list holding the car icon image and the spike icon
    uint8_t* spike = nullptr;       // spike strip icon (a clone of the car icon)
    uint8_t* heatIcon = nullptr;    // engine warning icon (a clone of the car icon), left of the spike icon
    uint8_t* aid[2] = {};           // ABS icon (above the engine warning icon) and TC icon (right of it)
    uint32_t aidTex[2] = {};        // their textures, 0 = none loaded (looked for again each second)
    double aidLooked = -1e9;
    uint32_t heatTex = 0;           // its texture hash, 0 = none loaded
    int heatPick = -1;              // which [HUD] HeatIcon name it is (0 = the first), -1 = none
    double heatLooked = -1e9;       // when a better one was last looked for
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
// filling up turns it from the opposite side, from -180 (empty) to 0 (full) as well, so 100%
// fills the whole arc up to the small arrow at its end (minAngle - 180 left the last 5 degrees
// empty: the bar never looked full).
float Angle(float level) {
    level = Clamp(level, 0.0f, 1.0f);
    if (!g_cfg.hudFillUp) return s.minAngle * (1.0f - level);
    float flip = s.minAngle > 0.0f ? -180.0f : 180.0f;
    return flip * (1.0f - level);
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

// The engine warning icon's texture: the first loaded one of [HUD] HeatIcon. ENGINE_HEAT_ICON comes
// from the drag HUD's pack, loaded alongside the race HUD's (LoadDragPack) but only after the HUD is
// built, so until it is there a later name stands in and the first is looked for again each second.
void PickHeatIcon() {
    s.heatLooked = Now();
    std::string names = g_cfg.hudHeatIcon;
    int i = 0;
    for (char* n = strtok(&names[0], ", "); n && (s.heatPick < 0 || i < s.heatPick); n = strtok(nullptr, ", "), i++) {
        uint32_t h = ((StringHash_t)kStringHash)(n);
        if (!((GetTextureInfo_t)kGetTextureInfo)(h, 0, 0)) continue;
        s.heatTex = h;
        s.heatPick = i;
        if (s.heatIcon && InList(s.iconList, s.heatIcon)) ((SetTextureHash_t)kSetTextureHash)(s.heatIcon, h);
        Log("hud: engine warning icon %s", n);
        return;
    }
    if (s.heatPick < 0) Log("hud: engine warning icon: none of %s loaded (yet)", g_cfg.hudHeatIcon.c_str());
}

void Setup(uint8_t* meter) {
    s.meter = meter;
    s.bar = Field<uint8_t*>(meter, kSB_Bar);
    s.list = s.clone = s.iconList = s.spike = s.heatIcon = s.pkg = nullptr;
    s.aid[0] = s.aid[1] = nullptr;
    s.arrow = {};
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
    s.pkg = pkg;
    s.pkgName = pkgName;
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
        // The smallest other image is the arrow at the arc's end: it glows with the car icon.
        size_t small = big;
        float smallArea = 1e30f;
        for (size_t i = 0; i < s.icon.size(); i++) {
            uint8_t* d = Field<uint8_t*>(s.icon[i].img, kFO_Data);
            float area = d ? fabsf(Field<float>(d, kOD_SizeX) * Field<float>(d, kOD_SizeY)) : 0.0f;
            if (i != big && area < smallArea) smallArea = area, small = i;
        }
        s.arrow = small != big ? s.icon[small] : IconImage{};
        if (!s.icon.empty()) s.icon = {s.icon[big]};
        // Clear of the arc's arrow: the car icon moved right (once per icon object; the spike strip
        // and engine warning icons are copies of it, placed relative to it, made after this).
        // Its colour as the game had it is kept from then on (we paint it every update after).
        static uint8_t* shifted;
        static uint32_t gameColor;
        uint8_t* d = !s.icon.empty() ? Field<uint8_t*>(s.icon[0].img, kFO_Data) : nullptr;
        if (d && s.icon[0].img == shifted) s.icon[0].color = gameColor;
        if (d && s.icon[0].img != shifted) {
            shifted = s.icon[0].img;
            gameColor = s.icon[0].color;
            Field<float>(d, kOD_PosX) += g_cfg.hudIconShift;
            Field<uint32_t>(s.icon[0].img, kFO_Flags) |= kDirty;
            uint8_t* a = s.arrow.img ? Field<uint8_t*>(s.arrow.img, kFO_Data) : nullptr;
            Log("hud: car icon moved %.1f right: at x %.1f (width %.1f), the arc's arrow at x %.1f (width %.1f)",
                g_cfg.hudIconShift, Field<float>(d, kOD_PosX), Field<float>(d, kOD_SizeX), a ? Field<float>(a, kOD_PosX) : 0.0f,
                a ? Field<float>(a, kOD_SizeX) : 0.0f);
        }
        if (!s.icon.empty()) s.iconList = FindList(icon + kFG_Children, s.icon[0].img);
    }
    if (!s.carIcon) s.carIcon = PickIcon();
    s.heatTex = 0;
    s.heatPick = -1;
    PickHeatIcon();
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

// The spike strip icon sits left of the engine warning icon (above the car icon) by its width plus
// the same gap.
float SideOffsetX() {
    uint8_t* d = Field<uint8_t*>(s.icon[0].img, kFO_Data);
    return d ? -fabsf(Field<float>(d, kOD_SizeX)) * (1.0f + kSpikeGap) : 0.0f;
}

void UpdateHeatIcon(bool show, float flash) {
    if (s.heatPick != 0 && Now() - s.heatLooked > 1.0) PickHeatIcon();
    if (!s.heatTex || !s.iconList || s.icon.empty()) return;
    if (!s.heatIcon || !InList(s.iconList, s.heatIcon)) {
        s.heatIcon = CloneIcon(0.0f, SpikeOffset(), 0.0f, s.heatTex);
        if (!s.heatIcon) {
            s.heatTex = 0;
            return;
        }
        Log("hud: engine warning icon %p added (y offset %.1f)", s.heatIcon, SpikeOffset());
    }
    SetColor(s.heatIcon, Bright(kHeatColor, flash));
    // Bit 0 = invisible: shown with the car icon, and only while the engine is hot or blown.
    uint32_t& flags = Field<uint32_t>(s.heatIcon, kFO_Flags);
    uint32_t vis = (flags & ~1u) | (Field<uint32_t>(s.icon[0].img, kFO_Flags) & 1u) | (show ? 0u : 1u);
    if (vis != flags) flags = vis | kDirty;
}

bool MakeSpike() {
    uint8_t* c = CloneIcon(SideOffsetX(), SpikeOffset(), 0.0f, s.spikeTex);
    if (!c) return false;
    s.spike = c;
    Log("hud: spike strip icon %p added (x offset %.1f, y offset %.1f)", c, SideOffsetX(), SpikeOffset());
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
    SetColor(s.spike, Bright(kHeatColor, flash));
    // Bit 0 = invisible (FEngSetInvisible): shown with the car icon, and only with a popped tire.
    uint32_t& flags = Field<uint32_t>(s.spike, kFO_Flags);
    uint32_t vis = (flags & ~1u) | (Field<uint32_t>(s.icon[0].img, kFO_Flags) & 1u) | (popped ? 0u : 1u);
    if (vis != flags) flags = vis | kDirty;
    if (!s.spikeOutline.empty() && InList(s.iconList, s.spikeOutline[0]))
        UpdateOutline(s.spikeOutline, SpikeOffset(), s.spikeTex, false);   // (unused: no outline)
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

// The ABS / TC icons' textures, from [HUD] AbsIcon / TcIcon (first loaded name wins).
uint32_t PickAidTex(const std::string& list, const char* what) {
    std::string names = list;
    for (char* n = strtok(&names[0], ", "); n; n = strtok(nullptr, ", ")) {
        uint32_t hash = ((StringHash_t)kStringHash)(n);
        if (((GetTextureInfo_t)kGetTextureInfo)(hash, 0, 0)) {
            Log("hud: %s icon texture %s", what, n);
            return hash;
        }
    }
    static int logged[2];
    int k = what[0] == 'A' ? 0 : 1;
    if (logged[k]++ < 3) Log("hud: %s icon: none of %s loaded in the race", what, list.c_str());
    return 0;
}

// ABS (above the engine warning icon) and TC (right of ABS): invisible while that aid is off, gray
// while it is on but idle, blinking in the HUD's beige while it works, like a real dash light.
void UpdateAidIcons(float wave) {
    if (!s.iconList || s.icon.empty()) return;
    if (Now() - s.aidLooked > 1.0 && (!s.aidTex[0] || !s.aidTex[1])) {
        s.aidLooked = Now();
        if (!s.aidTex[0]) s.aidTex[0] = PickAidTex(g_cfg.hudAbsIcon, "ABS");
        if (!s.aidTex[1]) s.aidTex[1] = PickAidTex(g_cfg.hudTcIcon, "TC");
    }
    bool active[2];
    Physics_Aids(&active[0], &active[1]);
    bool on[2] = {g_cfg.abs, g_cfg.tc};
    uint32_t beige = s.icon[0].color | 0xFF000000;
    for (int k = 0; k < 2; k++) {
        if (!s.aidTex[k]) continue;
        uint8_t*& img = s.aid[k];
        if (!img || !InList(s.iconList, img)) {
            img = CloneIcon(k == 0 ? 0.0f : -SideOffsetX(), 2.0f * SpikeOffset(), 0.0f, s.aidTex[k]);
            if (!img) {
                s.aidTex[k] = 0;
                continue;
            }
            Log("hud: %s icon %p added", k == 0 ? "ABS" : "TC", img);
        }
        // Placed from the car icon every update, so [HUD] AidIconRight / AidIconUp (F4 sliders) move
        // it live: its own spot (ABS above the engine warning icon, TC right of it) plus the offset.
        uint8_t* car = Field<uint8_t*>(s.icon[0].img, kFO_Data);
        uint8_t* d = Field<uint8_t*>(img, kFO_Data);
        if (car && d) {
            float x = Field<float>(car, kOD_PosX) + (k == 0 ? 0.0f : -SideOffsetX()) + g_cfg.hudAidRight;
            float y = Field<float>(car, kOD_PosY) + 2.0f * SpikeOffset() - g_cfg.hudAidUp;   // screen y runs down
            if (Field<float>(d, kOD_PosX) != x || Field<float>(d, kOD_PosY) != y) {
                Field<float>(d, kOD_PosX) = x;
                Field<float>(d, kOD_PosY) = y;
                Field<uint32_t>(img, kFO_Flags) |= kDirty;
            }
        }
        SetColor(img, active[k] ? Bright(beige, 0.2f + 0.8f * wave) : kIconGray);
        uint32_t& flags = Field<uint32_t>(img, kFO_Flags);
        uint32_t vis = (flags & ~1u) | (Field<uint32_t>(s.icon[0].img, kFO_Flags) & 1u) | (on[k] ? 0u : 1u);
        if (vis != flags) {
            flags = vis | kDirty;
            Log("hud: %s icon %s", k == 0 ? "ABS" : "TC",
                !(vis & 1u) ? "shown" : on[k] ? "hidden with the car icon" : "hidden (aid off)");
        }
    }
}

void Restore() {
    if (!s.applied) return;
    for (auto& i : s.icon) {
        ((SetTextureHash_t)kSetTextureHash)(i.img, i.tex);
        SetColor(i.img, i.color);
    }
    if (s.arrow.img) SetColor(s.arrow.img, s.arrow.color);
    if (s.bar) {
        SetColor(s.bar, s.barColor);
        SetRotation(s.bar, s.minAngle * (1.0f - Field<float>(s.meter, kSB_Iface + 8)));   // the game's level
    }
    if (s.clone && InList(s.list, s.clone)) SetColor(s.clone, WithAlpha(kHeatColor, 0.0f));
    if (s.spike && InList(s.iconList, s.spike)) SetColor(s.spike, 0);
    if (s.heatIcon && InList(s.iconList, s.heatIcon)) SetColor(s.heatIcon, 0);
    for (uint8_t* a : s.aid)
        if (a && InList(s.iconList, a)) SetColor(a, 0);
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
    // The car icon, no outline (an outlined, opaque copy looked harsh): gray undamaged, the
    // speedbreaker clock icon's own beige when damaged, and from IconRedFrom red, blinking by
    // brightness in the rhythm of the spike strip and engine warning icons (opaque: a translucent
    // icon takes on the arc's colour behind it).
    uint32_t beige = (s.icon.empty() ? s.barColor : s.icon[0].color) | 0xFF000000;
    uint32_t iconColor = h.damage >= kIconRedFrom ? Bright(kHeatColor, flash)
                         : h.damage >= kIconDamaged ? beige
                         : kIconGray;
    for (auto& i : s.icon)
        SetColor(i.img, iconColor);
    if (!s.carOutline.empty() && InList(s.iconList, s.carOutline[0]))
        UpdateOutline(s.carOutline, 0.0f, Field<uint32_t>(s.icon[0].img, kFO_Handle), false);
    UpdateSpike(h.poppedTires, flash);
    UpdateHeatIcon(h.heat >= g_cfg.hudHeatIconFrom || h.engineBlown, flash);
    UpdateAidIcons(wave);
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

// The pause menu: the game's own HUD goes, but what this file colours (the arc, its heat overlay,
// the car, spike strip, engine warning and ABS / TC icons) kept the colours the last update gave
// it. While the game time stands still they are all made transparent before the HUD is drawn;
// the next HUD update after the pause colours them again.
constexpr uintptr_t kSimTime = 0x9885D8;            // float: game time (Sim::GetTime), stops while paused
constexpr double kPausedAfter = 0.15;               // seconds of real time without the game time moving

bool Paused() {
    static float last = -1.0f;
    static double movedAt = 0;
    float t = *(volatile float*)kSimTime;
    if (t != last) {
        last = t;
        movedAt = Now();
    }
    return Now() - movedAt > kPausedAfter;
}

void HideForPause() {
    static bool hidden = false;
    // Only while the meter's HUD package is still loaded (the frontend unloads it; its objects are
    // gone then).
    uint8_t* feng = *(uint8_t**)kFEngInstance;
    bool alive = s.applied && s.pkg && feng && ((FindPackage_t)kFindPackage)(feng, s.pkgName.c_str()) == s.pkg &&
                 s.bar && *(uintptr_t*)s.bar == kVtblMultiImage;
    bool paused = Paused() && alive;
    if (paused != hidden) {
        hidden = paused;
        Log("hud: %s", paused ? "game paused: meter and icons hidden" : "game running: meter and icons back");
    }
    if (!paused) return;
    if (s.bar) SetColor(s.bar, 0);
    if (s.clone && InList(s.list, s.clone)) SetColor(s.clone, 0);
    for (auto& i : s.icon) SetColor(i.img, 0);
    for (uint8_t* c : {s.spike, s.heatIcon, s.aid[0], s.aid[1]})
        if (c && InList(s.iconList, c)) SetColor(c, 0);
}

// The game's Update turns the arc's mask when the level changes; ours is put back after it.
void __fastcall Hook_MeterUpdate(uint8_t* meter, void*, void* player) {
    ((MeterUpdate_t)kUpdate)(meter, player);
    if (g_cfg.hudMeter && s.applied && meter == s.meter && s.bar && Now() - s.seen < 0.5)
        SetRotation(s.bar, Angle(s.h.damage));
}

// The drag HUD's texture pack next to the race HUD's, for ENGINE_HEAT_ICON. The HUD's texture
// loader (a global at 0x8F5118) loads one pack per HUD type: Load(type, x) at 0x5A6540 (thiscall,
// ret 8; 2 = HUDTEXDRAG.BIN, else HUDTEXRACE.BIN for 1) through the game's resource files:
// CreateResourceFile(path, 3, 0, 0, 0), SetAllocationParams(0x2000, path), BeginLoading(callback,
// param). Unload(type) at 0x57C530 (thiscall, ret 4) unloads every HUD pack's textures by path,
// HUDTEXDRAG.BIN included, and frees its own file. So with a normal HUD (type 1) the drag pack
// is loaded too, with a callback that does nothing (the loader's own one counts its packs and
// builds the HUD on the first), and its file is freed after the loader's Unload.
constexpr uintptr_t kLoaderLoad = 0x5A6540, kCallLoaderLoad = 0x5A6742;
constexpr uintptr_t kLoaderUnload = 0x57C530, kCallLoaderUnload = 0x5A0BB5;
constexpr uintptr_t kDragPackPath = 0x8F7ED8;       // const char* "GLOBAL\HUDTEXDRAG.BIN"
constexpr uintptr_t kCreateResourceFile = 0x65FD30; // ResourceFile* (cdecl: path, type, flags, ?, ?)
constexpr uintptr_t kSetAllocationParams = 0x64ABE0; // thiscall (flags, debug name)
constexpr uintptr_t kBeginLoading = 0x6616F0;       // thiscall (callback(param), param)
constexpr uintptr_t kUnloadResourceFile = 0x663F00; // cdecl (ResourceFile*)
constexpr int kHudStandard = 1;

void* s_dragPack = nullptr;

void __cdecl DragPackLoaded(void*) { Log("hud: drag HUD textures loaded (engine warning icon)"); }

void FreeDragPack() {
    if (!s_dragPack) return;
    ((void(__cdecl*)(void*))kUnloadResourceFile)(s_dragPack);
    s_dragPack = nullptr;
}

// The mod's own HUD icons (the ABS and TC icons: PERFORMANCE_BRAKES and MAIN_ICON_QUICK_RACE, which
// only the front end's pack has): scripts\nfsmw_hud_icons.bin, a two-texture pack built from the
// game's FRONTEND\FrontB.lzc on the first start (iconpack.cpp; gameplay\tools\make_icon_pack.py
// makes the same file), laid out like HUDTEXDRAG.BIN.
// Loaded with every race HUD, its textures unloaded (by path, 0x5033C0, as the loader does for its
// own packs) and its file freed when the HUD's are.
constexpr const char* kIconPackPath = "scripts\\nfsmw_hud_icons.bin";
constexpr uintptr_t kUnloadTexturePack = 0x5033C0;  // cdecl (file path): the pack's textures unloaded
void* s_iconPack = nullptr;
bool s_iconPackLoaded = false;      // its textures are in (only then unloaded by path)

void __cdecl IconPackLoaded(void*) {
    s_iconPackLoaded = true;
    Log("hud: mod HUD icons loaded (%s)", kIconPackPath);
}

void LoadIconPack() {
    if (s_iconPack) return;
    if (GetFileAttributesA(kIconPackPath) == INVALID_FILE_ATTRIBUTES) {
        static bool logged;
        if (!logged) {
            logged = true;
            Log("hud: %s missing: no ABS / TC icons", kIconPackPath);
        }
        return;
    }
    void* file = ((void*(__cdecl*)(const char*, int, int, int, int))kCreateResourceFile)(kIconPackPath, 3, 0, 0, 0);
    if (!file) {
        Log("hud: mod HUD icons not loaded (no resource file)");
        return;
    }
    ((void(__fastcall*)(void*, void*, int, const char*))kSetAllocationParams)(file, nullptr, 0x2000, kIconPackPath);
    ((void(__fastcall*)(void*, void*, void*, void*))kBeginLoading)(file, nullptr, (void*)&IconPackLoaded, nullptr);
    s_iconPack = file;
}

void FreeIconPack() {
    if (!s_iconPack) return;
    if (s_iconPackLoaded) ((void(__cdecl*)(const char*))kUnloadTexturePack)(kIconPackPath);
    s_iconPackLoaded = false;
    ((void(__cdecl*)(void*))kUnloadResourceFile)(s_iconPack);
    s_iconPack = nullptr;
}

void __fastcall Hook_LoaderLoad(void* loader, void* edx, int type, int extra) {
    ((void(__fastcall*)(void*, void*, int, int))kLoaderLoad)(loader, edx, type, extra);
    LoadIconPack();
    if (type != kHudStandard || !g_cfg.hudMeter || g_cfg.hudHeatIcon.find("ENGINE_HEAT_ICON") == std::string::npos) return;
    if (s_dragPack) return;   // still loaded (its textures may be in use): kept, never freed under them
    const char* path = *(const char**)kDragPackPath;
    void* file = ((void*(__cdecl*)(const char*, int, int, int, int))kCreateResourceFile)(path, 3, 0, 0, 0);
    if (!file) {
        Log("hud: drag HUD textures not loaded (no resource file)");
        return;
    }
    ((void(__fastcall*)(void*, void*, int, const char*))kSetAllocationParams)(file, nullptr, 0x2000, path);
    ((void(__fastcall*)(void*, void*, void*, void*))kBeginLoading)(file, nullptr, (void*)&DragPackLoaded, nullptr);
    s_dragPack = file;
}

void __fastcall Hook_LoaderUnload(void* loader, void* edx, int type) {
    ((void(__fastcall*)(void*, void*, int))kLoaderUnload)(loader, edx, type);
    FreeDragPack();
    FreeIconPack();
}

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

}  // namespace

void Hud_Frame() { HideForPause(); }

bool Hud_Message(const char* text) {
    if (!s.meter || Now() - s.seen > 0.5) return false;
    void* outer = Field<void*>(s.meter + kSB_Iface, 4);    // IUnknown -> its UTL::COM::Object
    void* gm = outer ? ((IListFind_t)kIListFind)(outer, (void*)kIGenericMessageHandle) : nullptr;
    // GenericMessage's own vtable (its RequestGenericMessage slot is the flow module's hook).
    if (!gm || (uintptr_t)Vtbl(gm) != kVtblGenericMessage) {
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
    // The ABS / TC icons' pack, made from the game's own files the first time (it isn't shipped).
    char iconPack[MAX_PATH];
    snprintf(iconPack, sizeof(iconPack), "%s%s", GameDir(), kIconPackPath);
    if (GetFileAttributesA(iconPack) == INVALID_FILE_ATTRIBUTES) {
        std::string error;
        if (IconPack_Build(GameDir(), &error)) Log("hud: %s made from FRONTEND\\FrontB.lzc (ABS / TC icons)", kIconPackPath);
        else Log("hud: %s not made (%s): no ABS / TC icons", kIconPackPath, error.c_str());
    }
    const char* path = *(const char**)kDragPackPath;
    if (path && !IsBadStringPtrA(path, 32) && strstr(path, "HUDTEXDRAG") &&
        RetargetCall(kCallLoaderUnload, kLoaderUnload, (void*)&Hook_LoaderUnload)) {
        if (RetargetCall(kCallLoaderLoad, kLoaderLoad, (void*)&Hook_LoaderLoad))
            Log("hud: drag HUD textures loaded with the race HUD's (engine warning icon)");
    } else {
        Log("hud: unexpected HUD texture loader, ENGINE_HEAT_ICON only in drag races");
    }
}
