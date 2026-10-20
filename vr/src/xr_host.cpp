// nfsmw_vr_host.exe: 64-bit OpenXR host for runtimes without 32-bit support (SteamVR).
//
// d3d9.dll starts it with the game's process id and forwards its Xr_* calls over shared
// memory (xr_bridge.h); this process runs them with the same xr.cpp, built for x64. It exits
// when the game does.
#include "xr_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmdLine, int) {
    LogInit("nfsmw_vr_host.log");
    LoadConfig();
    DWORD pid = strtoul(cmdLine, nullptr, 10);

    char name[64];
    snprintf(name, sizeof(name), XR_BRIDGE_MAPPING, pid);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    snprintf(name, sizeof(name), XR_BRIDGE_REQUEST, pid);
    HANDLE request = OpenEventA(SYNCHRONIZE, FALSE, name);
    snprintf(name, sizeof(name), XR_BRIDGE_REPLY, pid);
    HANDLE reply = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, pid);
    auto* shm = mapping ? (BridgeShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeShared)) : nullptr;
    if (!shm || !request || !reply || !game) {
        Log("host: cannot attach to game process %lu (%lu). This program is started by d3d9.dll.", pid, GetLastError());
        return 1;
    }
    Log("NFSMW VR host started for game process %lu", pid);

    HANDLE wait[2] = {request, game};
    unsigned frames = 0;
    for (bool running = true; running;) {
        // The game stops calling while it is paused or minimised; keep answering the runtime
        // (SteamVR kills apps that ignore a quit request for 5 s).
        DWORD w = WaitForMultipleObjects(2, wait, FALSE, 100);
        if (w == WAIT_TIMEOUT) {
            Xr_PollEvents();
            continue;
        }
        if (w != WAIT_OBJECT_0) {
            Log("host: game exited");
            break;
        }
        switch (shm->cmd) {
        case kBridgeInit: {
            shm->hostProtocolVersion = kBridgeProtocolVersion;
            if (shm->protocolVersion != kBridgeProtocolVersion) {
                Log("host: protocol mismatch; install the VR plugin and host from the same build");
                shm->result = 0;
                break;
            }
            LUID luid = {shm->luidLow, shm->luidHigh};
            shm->result = Xr_Init(luid);
            Xr_RecommendedSize(&shm->recW, &shm->recH);
            strncpy(shm->runtimeName, Xr_RuntimeName(), sizeof(shm->runtimeName) - 1);
            break;
        }
        case kBridgeShutdown:
            Xr_Shutdown();
            running = false;
            break;
        case kBridgeBeginFrame:
            shm->result = Xr_BeginFrame();
            shm->frameBegun = Xr_FrameBegun();
            shm->shouldRender = shm->result;
            shm->eyes[0] = Xr_Eye(0);
            shm->eyes[1] = Xr_Eye(1);
            break;
        case kBridgeSetTextures: {
            HANDLE h[3];
            for (int i = 0; i < 3; i++) h[i] = (HANDLE)(intptr_t)(int32_t)shm->handles[i];
            shm->result = Xr_SetSharedTextures(h[0], h[1], h[2], shm->width, shm->height);
            break;
        }
        case kBridgeEndFrame:
            g_cfg.hudDistance = shm->hudDistance;
            g_cfg.hudWidth = shm->hudWidth;
            g_cfg.hudHeight = shm->hudHeight;
            g_cfg.hudRight = shm->hudRight;
            g_cfg.screenDistance = shm->screenDistance;
            g_cfg.screenWidth = shm->screenWidth;
            Xr_EndFrame((LayerMode)shm->mode, shm->tanX, shm->tanY, shm->withHud != 0);
            frames++;
            break;
        case kBridgeRecenter:
            Xr_Recenter();
            break;
        }
        SetEvent(reply);
    }
    Xr_Shutdown();
    Log("host: exiting after %u frames", frames);
    return 0;
}
