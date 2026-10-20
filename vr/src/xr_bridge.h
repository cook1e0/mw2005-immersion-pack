// Shared-memory protocol between the 32-bit game (d3d9.dll) and nfsmw_vr_host.exe, a 64-bit
// process that runs the OpenXR session for runtimes without 32-bit support (SteamVR).
//
// The game writes a command and its arguments, signals the request event and waits for the
// reply event. Eye images travel as D3D9Ex shared texture handles (global, valid in any process).
#pragma once
#include "xr.h"

#define XR_BRIDGE_EXE "nfsmw_vr_host.exe"
constexpr uint32_t kBridgeProtocolVersion = 2;

// Object names, formatted with the game's process id.
#define XR_BRIDGE_MAPPING "Local\\nfsmw_vr_bridge_%lu"
#define XR_BRIDGE_REQUEST "Local\\nfsmw_vr_bridge_%lu_req"
#define XR_BRIDGE_REPLY   "Local\\nfsmw_vr_bridge_%lu_rep"

enum BridgeCmd : int32_t {
    kBridgeInit = 1,
    kBridgeShutdown,
    kBridgeBeginFrame,
    kBridgeSetTextures,
    kBridgeEndFrame,
    kBridgeRecenter,
};

// Only fixed-size types, so the layout is identical in the 32- and 64-bit builds.
struct BridgeShared {
    int32_t cmd;
    int32_t result;

    // Init
    uint32_t luidLow;
    int32_t  luidHigh;
    int32_t  recW, recH;
    char     runtimeName[128];

    // BeginFrame
    int32_t frameBegun;
    int32_t shouldRender;
    EyeView eyes[2];

    // SetTextures (D3D9Ex shared handles are 32-bit values in both processes)
    uint32_t handles[3];
    int32_t  width, height;

    // EndFrame
    int32_t mode;
    float   tanX, tanY;
    int32_t withHud;
    float   hudDistance, hudWidth, hudHeight;   // live from the game (gameplay mod's F4 menu)
    float   screenDistance, screenWidth;
    // Appended to keep command/result offsets stable for a helpful old-host error.
    uint32_t protocolVersion, hostProtocolVersion;
    float   hudRight;                           // appended: sideways offset of the HUD panel
};

static_assert(sizeof(EyeView) == 18 * 4, "EyeView must be plain floats");
