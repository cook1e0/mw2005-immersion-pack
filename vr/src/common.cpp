#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

Config g_cfg;

static char s_gameDir[MAX_PATH];
static FILE* s_log;
static CRITICAL_SECTION s_logLock;

const char* GameDir() {
    if (!s_gameDir[0]) {
        GetModuleFileNameA(nullptr, s_gameDir, MAX_PATH);
        char* slash = strrchr(s_gameDir, '\\');
        if (slash) slash[1] = 0;
    }
    return s_gameDir;
}

void LogInit() {
    InitializeCriticalSection(&s_logLock);
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%snfsmw_vr.log", GameDir());
    s_log = fopen(path, "w");
}

void Log(const char* fmt, ...) {
    if (!s_log) return;
    EnterCriticalSection(&s_logLock);
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(s_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s_log, fmt, ap);
    va_end(ap);
    fputc('\n', s_log);
    fflush(s_log);
    LeaveCriticalSection(&s_logLock);
}

static float ReadFloat(const char* ini, const char* key, float def) {
    char buf[64], defs[64];
    snprintf(defs, sizeof(defs), "%g", def);
    GetPrivateProfileStringA("VR", key, defs, buf, sizeof(buf), ini);
    return (float)atof(buf);
}

static int ReadInt(const char* ini, const char* key, int def) {
    return (int)GetPrivateProfileIntA("VR", key, def, ini);
}

void LoadConfig() {
    char ini[MAX_PATH];
    snprintf(ini, sizeof(ini), "%snfsmw_vr.ini", GameDir());
    Config& c = g_cfg;
    c.enabled = ReadInt(ini, "Enabled", c.enabled) != 0;
    c.forceWindowed = ReadInt(ini, "ForceWindowed", c.forceWindowed) != 0;
    c.disableVSync = ReadInt(ini, "DisableVSync", c.disableVSync) != 0;
    c.worldScale = ReadFloat(ini, "WorldScale", c.worldScale);
    c.fovScale = ReadFloat(ini, "FovScale", c.fovScale);
    GetPrivateProfileStringA("VR", "Resolution", c.resolution, c.resolution, sizeof(c.resolution), ini);
    c.resolutionScale = ReadFloat(ini, "ResolutionScale", c.resolutionScale);
    c.levelRoll = ReadFloat(ini, "LevelRoll", c.levelRoll);
    c.levelPitch = ReadFloat(ini, "LevelPitch", c.levelPitch);
    c.screenDistance = ReadFloat(ini, "ScreenDistance", c.screenDistance);
    c.screenWidth = ReadFloat(ini, "ScreenWidth", c.screenWidth);
    c.hudDistance = ReadFloat(ini, "HudDistance", c.hudDistance);
    c.hudWidth = ReadFloat(ini, "HudWidth", c.hudWidth);
    c.hudHeight = ReadFloat(ini, "HudHeight", c.hudHeight);
    c.fakeHmd = ReadInt(ini, "FakeHmd", c.fakeHmd) != 0;
    c.fakeYaw = ReadFloat(ini, "FakeYaw", c.fakeYaw);
    c.fakePitch = ReadFloat(ini, "FakePitch", c.fakePitch);
    c.fakeIpd = ReadFloat(ini, "FakeIpd", c.fakeIpd);
    c.dumpFrames = ReadInt(ini, "DumpFrames", c.dumpFrames);
    c.keyRecenter = ReadInt(ini, "KeyRecenter", c.keyRecenter);
    c.keyDump = ReadInt(ini, "KeyDump", c.keyDump);
    c.keyToggle = ReadInt(ini, "KeyToggle", c.keyToggle);
    c.keyLevel = ReadInt(ini, "KeyLevel", c.keyLevel);
    c.keySpectator = ReadInt(ini, "KeySpectator", c.keySpectator);
    GetPrivateProfileStringA("VR", "SpectatorMode", c.spectatorMode, c.spectatorMode, sizeof(c.spectatorMode), ini);
    c.spectatorEye = ReadInt(ini, "SpectatorEye", c.spectatorEye) != 0 ? 1 : 0;
    c.spectatorFov = ReadFloat(ini, "SpectatorFov", c.spectatorFov);
    c.spectatorAspect = ReadFloat(ini, "SpectatorAspect", c.spectatorAspect);
    c.spectatorSmoothing = ReadFloat(ini, "SpectatorSmoothing", c.spectatorSmoothing);
    c.spectatorFollowHead = ReadInt(ini, "SpectatorFollowHead", c.spectatorFollowHead) != 0;
    c.spectatorWidth = ReadInt(ini, "SpectatorWidth", c.spectatorWidth);
    Log("config: enabled=%d windowed=%d novsync=%d worldScale=%.3f fovScale=%.3f levelRoll=%.2f levelPitch=%.2f fakeHmd=%d",
        c.enabled, c.forceWindowed, c.disableVSync, c.worldScale, c.fovScale, c.levelRoll, c.levelPitch, c.fakeHmd);
}
