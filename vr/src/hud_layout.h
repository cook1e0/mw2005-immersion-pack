// The VR HUD layout: drawn by the VR mod, set in the gameplay mod's F4 menu (VR page) through
// NfsmwVr_Setting("Hud...") and kept in nfsmw_vr.ini [VR] Hud* keys.
//
// In the headset the game's HUD is cut into elements, each a crop of the HUD image placed in the
// cockpit: the black box (cop UI, leaderboard and location text stacked on a dark panel), the
// speedometer, the radar and the minimap. Crops are in the HUD image (0..1, x right, y down);
// places are in the seat's space (metres: x right, y up, z forward from the driver's eyes), with
// the element's width. An element set to occlude is drawn with the scene's depth, so the steering
// wheel and dashboard in front of it hide it.
#pragma once

struct HudCrop {
    float x, y, w, h;
};

struct HudPlace {
    float x, y, z;      // seat space, metres
    float width;        // metres
    float occlude;      // 1 = behind the 3D steering wheel and dashboard (scene depth)
};

enum HudCropId { HUD_COP, HUD_LEADERBOARD, HUD_LOCATION, HUD_SPEEDO, HUD_RADAR, HUD_MINIMAP, HUD_NUM_CROPS };
enum HudElementId { HUD_BOX, HUD_E_SPEEDO, HUD_E_RADAR, HUD_E_MINIMAP, HUD_NUM_ELEMENTS };

struct HudLayout {
    float enabled;                  // 1 = the HUD as elements in the cockpit (driving); 0 = the old panel
    float setup;                    // unused
    float menuOpen;                 // 1 = the F4 menu is open (gameplay mod): the panel is shown
    float boxOpacity;               // the black box's background
    float selected;                 // element outlined while its F4 row is selected (-1 none)
    HudCrop crop[HUD_NUM_CROPS];
    HudPlace place[HUD_NUM_ELEMENTS];
};

// Element -> its crops (the black box stacks three).
inline int HudElementCrops(int element, int out[3]) {
    switch (element) {
    case HUD_BOX: out[0] = HUD_COP; out[1] = HUD_LEADERBOARD; out[2] = HUD_LOCATION; return 3;
    case HUD_E_SPEEDO: out[0] = HUD_SPEEDO; return 1;
    case HUD_E_RADAR: out[0] = HUD_RADAR; return 1;
    case HUD_E_MINIMAP: out[0] = HUD_MINIMAP; return 1;
    }
    return 0;
}

inline const char* HudCropName(int c) {
    static const char* n[HUD_NUM_CROPS] = {"Cop UI", "Leaderboard", "Location", "Speedometer", "Radar", "Minimap"};
    return c >= 0 && c < HUD_NUM_CROPS ? n[c] : "?";
}
inline const char* HudCropKey(int c) {
    static const char* n[HUD_NUM_CROPS] = {"CopUI", "Leaderboard", "Location", "Speedometer", "Radar", "Minimap"};
    return c >= 0 && c < HUD_NUM_CROPS ? n[c] : "?";
}
inline const char* HudElementName(int e) {
    static const char* n[HUD_NUM_ELEMENTS] = {"Black box", "Speedometer", "Radar", "Minimap"};
    return e >= 0 && e < HUD_NUM_ELEMENTS ? n[e] : "?";
}
inline const char* HudElementKey(int e) {
    static const char* n[HUD_NUM_ELEMENTS] = {"Box", "Speedometer", "Radar", "Minimap"};
    return e >= 0 && e < HUD_NUM_ELEMENTS ? n[e] : "?";
}

inline HudLayout HudDefaults() {
    HudLayout l = {};
    l.enabled = 0.0f;
    l.boxOpacity = 0.65f;
    l.selected = -1;
    l.crop[HUD_COP] = {0.30f, 0.84f, 0.40f, 0.16f};
    l.crop[HUD_LEADERBOARD] = {0.00f, 0.0408f, 0.30f, 0.30f};
    l.crop[HUD_LOCATION] = {0.30f, 0.74f, 0.40f, 0.08f};
    l.crop[HUD_SPEEDO] = {0.4796f, 0.3116f, 0.30f, 0.38f};
    l.crop[HUD_RADAR] = {0.72f, 0.5684f, 0.28f, 0.10f};
    l.crop[HUD_MINIMAP] = {0.0068f, 0.2713f, 0.28f, 0.38f};
    l.place[HUD_BOX] = {0.00f, 0.10f, 0.85f, 0.32f, 0.0f};
    l.place[HUD_E_SPEEDO] = {0.22f, -0.28f, 0.60f, 0.16f, 0.0f};
    l.place[HUD_E_RADAR] = {0.00f, -0.20f, 0.65f, 0.14f, 0.0f};
    l.place[HUD_E_MINIMAP] = {-0.22f, -0.28f, 0.60f, 0.16f, 0.0f};
    return l;
}
