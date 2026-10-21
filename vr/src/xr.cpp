#include "xr.h"
#include "xr_bridge.h"

#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <string>
#include <vector>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_NO_PROTOTYPES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

// ---------------------------------------------------------------------------
// Function table (no loader DLL: we negotiate with the active runtime directly)
// ---------------------------------------------------------------------------
#define XR_FUNCS(X)                                  \
    X(xrEnumerateInstanceExtensionProperties)        \
    X(xrCreateInstance)                              \
    X(xrDestroyInstance)                             \
    X(xrGetInstanceProperties)                       \
    X(xrResultToString)                              \
    X(xrGetSystem)                                   \
    X(xrGetD3D11GraphicsRequirementsKHR)             \
    X(xrCreateSession)                               \
    X(xrDestroySession)                              \
    X(xrBeginSession)                                \
    X(xrEndSession)                                  \
    X(xrCreateReferenceSpace)                        \
    X(xrLocateSpace)                                 \
    X(xrEnumerateViewConfigurationViews)             \
    X(xrEnumerateSwapchainFormats)                   \
    X(xrCreateSwapchain)                             \
    X(xrDestroySwapchain)                            \
    X(xrEnumerateSwapchainImages)                    \
    X(xrAcquireSwapchainImage)                       \
    X(xrWaitSwapchainImage)                          \
    X(xrReleaseSwapchainImage)                       \
    X(xrPollEvent)                                   \
    X(xrWaitFrame)                                   \
    X(xrBeginFrame)                                  \
    X(xrEndFrame)                                    \
    X(xrLocateViews)

#define DECLARE_FN(name) static PFN_##name name;
XR_FUNCS(DECLARE_FN)
#undef DECLARE_FN

static PFN_xrGetInstanceProcAddr s_gipa;

namespace {

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    std::vector<ID3D11RenderTargetView*> rtvs;
    int width = 0, height = 0;
};

struct State {
    HMODULE runtime = nullptr;
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool fake = false;
    bool remote = false;     // session runs in nfsmw_vr_host.exe
    bool loadFailed = false; // no 32-bit runtime could be loaded
    char runtimeName[128] = {};

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11Buffer* cbuf = nullptr;

    int64_t format = 0;
    bool formatSrgb = false;
    Swapchain chains[3]; // left, right, HUD

    ID3D11Texture2D* eyeTex[3] = {};
    ID3D11ShaderResourceView* eyeSrv[3] = {};
    int eyeW = 0, eyeH = 0;
    int recW = 0, recH = 0;

    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    bool frameBegun = false;
    bool viewsValid = false;
    EyeView eyes[2] = {};

    bool haveOrigin = false;
    bool recenterPending = true;
    Quat originRot = {0, 0, 0, 1};
    V3 originPos = {0, 0, 0};
};
State s;

const char* ResultStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (xrResultToString && s.instance && XR_SUCCEEDED(xrResultToString(s.instance, r, buf))) return buf;
    snprintf(buf, sizeof(buf), "XrResult(%d)", (int)r);
    return buf;
}

#define XR_CHECK(call)                                              \
    do {                                                            \
        XrResult r_ = (call);                                       \
        if (XR_FAILED(r_)) {                                        \
            Log("XR: %s failed: %s", #call, ResultStr(r_));         \
            return false;                                           \
        }                                                           \
    } while (0)

// ---------------------------------------------------------------------------
// Runtime discovery: XR_RUNTIME_JSON env var, else HKLM\SOFTWARE\Khronos\OpenXR\1\ActiveRuntime
// (a 32-bit process is transparently redirected to the WOW6432Node key).
// ---------------------------------------------------------------------------
std::string FindRuntimeJson() {
    char buf[MAX_PATH * 2] = {};
    if (GetEnvironmentVariableA("XR_RUNTIME_JSON", buf, sizeof(buf)) > 0) return buf;
    DWORD size = sizeof(buf);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return {};
}

std::string ReadLibraryPath(const std::string& jsonPath) {
    FILE* f = fopen(jsonPath.c_str(), "rb");
    if (!f) return {};
    std::string text;
    char chunk[1024];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) text.append(chunk, n);
    fclose(f);

    size_t key = text.find("\"library_path\"");
    if (key == std::string::npos) return {};
    size_t q1 = text.find('"', text.find(':', key) + 1);
    if (q1 == std::string::npos) return {};
    std::string value;
    for (size_t i = q1 + 1; i < text.size() && text[i] != '"'; i++) {
        if (text[i] == '\\' && i + 1 < text.size()) i++; // JSON escape: \\ -> '\'
        value += text[i];
    }
    bool absolute = value.size() > 1 && (value[1] == ':' || (value[0] == '\\' && value[1] == '\\'));
    if (!absolute) {
        std::string dir = jsonPath.substr(0, jsonPath.find_last_of("\\/") + 1);
        value = dir + value;
    }
    return value;
}

bool LoadRuntime() {
    std::string json = FindRuntimeJson();
    if (json.empty()) {
        Log("XR: no active OpenXR runtime registered");
        return false;
    }
    std::string lib = ReadLibraryPath(json);
    Log("XR: runtime manifest %s -> %s", json.c_str(), lib.c_str());
    s.runtime = LoadLibraryA(lib.c_str());
    if (!s.runtime) {
        Log("XR: LoadLibrary failed (%lu). Is this runtime available for 32-bit apps?", GetLastError());
        return false;
    }
    auto negotiate = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(s.runtime, "xrNegotiateLoaderRuntimeInterface");
    if (!negotiate) {
        Log("XR: runtime does not export xrNegotiateLoaderRuntimeInterface");
        return false;
    }
    XrNegotiateLoaderInfo info = {};
    info.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
    info.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
    info.structSize = sizeof(info);
    info.minInterfaceVersion = 1;
    info.maxInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    info.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
    info.maxApiVersion = XR_MAKE_VERSION(1, 0x3ff, 0xfff);
    XrNegotiateRuntimeRequest req = {};
    req.structType = XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST;
    req.structVersion = XR_RUNTIME_INFO_STRUCT_VERSION;
    req.structSize = sizeof(req);
    XrResult r = negotiate(&info, &req);
    if (XR_FAILED(r) || !req.getInstanceProcAddr) {
        Log("XR: runtime negotiation failed (%d)", (int)r);
        return false;
    }
    s_gipa = req.getInstanceProcAddr;
    s_gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", (PFN_xrVoidFunction*)&xrEnumerateInstanceExtensionProperties);
    s_gipa(XR_NULL_HANDLE, "xrCreateInstance", (PFN_xrVoidFunction*)&xrCreateInstance);
    return xrEnumerateInstanceExtensionProperties && xrCreateInstance;
}

bool LoadInstanceFunctions() {
#define LOAD_FN(name)                                                                      \
    if (XR_FAILED(s_gipa(s.instance, #name, (PFN_xrVoidFunction*)&name)) || !name) {       \
        Log("XR: missing function " #name);                                                \
        return false;                                                                      \
    }
    XR_FUNCS(LOAD_FN)
#undef LOAD_FN
    return true;
}

// ---------------------------------------------------------------------------
// D3D11: device on the headset's adapter + a tiny fullscreen-triangle blit.
// ---------------------------------------------------------------------------
const char* kBlitHlsl = R"(
Texture2D src : register(t0);
SamplerState smp : register(s0);
cbuffer Params : register(b0) { float decodeSrgb; float keepAlpha; float2 pad; };
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut vs_main(uint id : SV_VertexID) {
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uv;
    return o;
}
float4 ps_main(VSOut i) : SV_Target {
    float4 s = src.Sample(smp, i.uv);
    float3 c = s.rgb;
    // The game's output is already gamma-encoded. When the swapchain is sRGB the
    // hardware will encode on write, so undo that here to keep the bytes identical.
    if (decodeSrgb > 0.5)
        c = (c <= 0.04045) ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    return float4(c, keepAlpha > 0.5 ? s.a : 1.0);
}
)";

bool CreateBlitPipeline() {
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    auto compile = dc ? (pD3DCompile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) {
        Log("XR: d3dcompiler_47.dll not available");
        return false;
    }
    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "vs_main", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(compile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "ps_main", "ps_4_0", 0, 0, &psb, &err))) {
        Log("XR: blit shader compile failed: %s", err ? (const char*)err->GetBufferPointer() : "?");
        return false;
    }
    s.dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &s.vs);
    s.dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &s.ps);
    vsb->Release();
    psb->Release();

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    s.dev->CreateSamplerState(&sd, &s.sampler);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    s.dev->CreateBuffer(&bd, nullptr, &s.cbuf);
    return s.vs && s.ps && s.sampler && s.cbuf;
}

bool CreateD3D11(LUID luid, D3D_FEATURE_LEVEL minLevel) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        adapter->GetDesc1(&d);
        if (d.AdapterLuid.LowPart == luid.LowPart && d.AdapterLuid.HighPart == luid.HighPart) {
            Log("XR: headset adapter: %ls", d.Description);
            break;
        }
        adapter->Release();
        adapter = nullptr;
    }
    factory->Release();
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT count = 0;
    while (count < 4 && levels[count] >= minLevel) count++;
    HRESULT hr = D3D11CreateDevice(adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, count ? count : 4, D3D11_SDK_VERSION,
                                   &s.dev, nullptr, &s.ctx);
    if (adapter) adapter->Release();
    if (FAILED(hr)) {
        Log("XR: D3D11CreateDevice failed 0x%08lx", hr);
        return false;
    }
    return CreateBlitPipeline();
}

// ---------------------------------------------------------------------------
// Swapchains
// ---------------------------------------------------------------------------
void DestroySwapchain(Swapchain& sc) {
    for (auto* v : sc.rtvs) v->Release();
    sc.rtvs.clear();
    if (sc.handle) xrDestroySwapchain(sc.handle);
    sc.handle = XR_NULL_HANDLE;
    sc.width = sc.height = 0;
}

bool PickFormat() {
    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(s.session, 0, &n, nullptr));
    std::vector<int64_t> formats(n);
    XR_CHECK(xrEnumerateSwapchainFormats(s.session, n, &n, formats.data()));
    const int64_t preferred[] = {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM};
    for (int64_t want : preferred)
        for (int64_t f : formats)
            if (f == want) {
                s.format = f;
                s.formatSrgb = (f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
                Log("XR: swapchain format %lld (srgb=%d)", (long long)f, s.formatSrgb);
                return true;
            }
    Log("XR: no supported swapchain format");
    return false;
}

bool CreateSwapchain(Swapchain& sc, int w, int h) {
    DestroySwapchain(sc);
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = s.format;
    ci.sampleCount = 1;
    ci.width = w;
    ci.height = h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(s.session, &ci, &sc.handle));
    uint32_t n = 0;
    XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &n, nullptr));
    std::vector<XrSwapchainImageD3D11KHR> images(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XR_CHECK(xrEnumerateSwapchainImages(sc.handle, n, &n, (XrSwapchainImageBaseHeader*)images.data()));
    for (auto& img : images) {
        D3D11_RENDER_TARGET_VIEW_DESC rd = {};
        rd.Format = (DXGI_FORMAT)s.format;
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(s.dev->CreateRenderTargetView(img.texture, &rd, &rtv))) {
            Log("XR: CreateRenderTargetView failed");
            return false;
        }
        sc.rtvs.push_back(rtv);
    }
    sc.width = w;
    sc.height = h;
    Log("XR: swapchain %dx%d with %u images", w, h, n);
    return true;
}

// ---------------------------------------------------------------------------
// Events / session lifecycle
// ---------------------------------------------------------------------------
void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = (XrEventDataSessionStateChanged*)&ev;
            s.state = e->state;
            Log("XR: session state -> %d", (int)s.state);
            if (s.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(s.session, &bi);
                s.running = XR_SUCCEEDED(r);
                s.recenterPending = true;
                Log("XR: xrBeginSession -> %s", ResultStr(r));
            } else if (s.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(s.session);
                s.running = false;
            } else if (s.state == XR_SESSION_STATE_EXITING || s.state == XR_SESSION_STATE_LOSS_PENDING) {
                s.running = false;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            s.recenterPending = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Yaw-only origin taken from the current head pose.
void UpdateOrigin(XrTime time) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(s.view, s.local, time, &loc))) return;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return;
    Quat q = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
    V3 fwd = QRotate(q, {0, 0, -1});
    float yaw = atan2f(-fwd.x, -fwd.z);
    s.originRot = QFromAxisAngle({0, 1, 0}, yaw);
    s.originPos = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
    s.haveOrigin = true;
    s.recenterPending = false;
    Log("XR: recentred (yaw %.1f deg)", yaw * 57.2958f);
}

void FillFakeEyes() {
    const float deg = 3.14159265f / 180;
    Quat head = QMul(QFromAxisAngle({0, 1, 0}, g_cfg.fakeYaw * deg), QFromAxisAngle({1, 0, 0}, g_cfg.fakePitch * deg));
    for (int i = 0; i < 2; i++) {
        EyeView& e = s.eyes[i];
        e.orientation = e.relOrientation = head;
        e.position = e.relPosition = QRotate(head, {(i == 0 ? -0.5f : 0.5f) * g_cfg.fakeIpd, 0, 0});
        e.tanLeft = e.tanRight = 1.0f;
        e.tanUp = e.tanDown = 1.0f;
    }
}

#ifndef NFSMW_XR_HOST
// ---------------------------------------------------------------------------
// Bridge client: the 64-bit nfsmw_vr_host.exe runs the OpenXR session for runtimes that have
// no 32-bit version (SteamVR). See xr_bridge.h.
// ---------------------------------------------------------------------------
struct Bridge {
    HANDLE mapping, request, reply, process;
    BridgeShared* shm;
    bool dead;
    bool pending;   // an EndFrame was posted and its reply not yet taken ([VR] AsyncSubmit)
};
Bridge b;

std::string ReadActiveRuntime(REGSAM view) {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", 0, KEY_READ | view, &key) != ERROR_SUCCESS)
        return {};
    char buf[MAX_PATH * 2] = {};
    DWORD size = sizeof(buf) - 1, type = 0;
    LONG r = RegQueryValueExA(key, "ActiveRuntime", nullptr, &type, (BYTE*)buf, &size);
    RegCloseKey(key);
    return r == ERROR_SUCCESS && type == REG_SZ ? buf : std::string();
}

std::string DirOf(const std::string& path) {
    size_t i = path.find_last_of("\\/");
    return i == std::string::npos ? std::string() : path.substr(0, i);
}

// Bridge=auto: use the bridge when the active runtime (the 64-bit registration, which runtime
// switchers such as SteamVR's set) has no 32-bit registration from the same install.
bool WantBridge() {
    if (g_cfg.bridge >= 0) return g_cfg.bridge == 1;
    char env[8];
    if (GetEnvironmentVariableA("XR_RUNTIME_JSON", env, sizeof(env)) > 0) return false;
    std::string rt64 = ReadActiveRuntime(KEY_WOW64_64KEY), rt32 = ReadActiveRuntime(KEY_WOW64_32KEY);
    Log("XR: active runtime: 64-bit \"%s\", 32-bit \"%s\"", rt64.c_str(), rt32.c_str());
    if (rt64.empty()) return false;
    return rt32.empty() || _stricmp(DirOf(rt64).c_str(), DirOf(rt32).c_str()) != 0;
}

bool BridgeWaitReply(DWORD timeoutMs) {
    HANDLE wait[2] = {b.reply, b.process};
    DWORD r = WaitForMultipleObjects(2, wait, FALSE, timeoutMs);
    if (r == WAIT_OBJECT_0) return true;
    Log(r == WAIT_OBJECT_0 + 1 ? "XR: bridge process exited (see nfsmw_vr_host.log)" : "XR: bridge not responding");
    b.dead = true;
    return false;
}

// The previous posted command's reply first: the shared memory and the eye textures are the
// host's until it answers.
bool BridgeTakePending(DWORD timeoutMs) {
    if (!b.pending) return true;
    b.pending = false;
    return BridgeWaitReply(timeoutMs);
}

bool BridgeCall(BridgeCmd cmd, DWORD timeoutMs = INFINITE) {
    if (!b.shm || b.dead || !BridgeTakePending(timeoutMs)) return false;
    b.shm->cmd = cmd;
    SetEvent(b.request);
    return BridgeWaitReply(timeoutMs);
}

// Posts the command and returns; the next BridgeCall (the next frame's BeginFrame, before any eye
// is drawn again) waits for its reply.
bool BridgePost(BridgeCmd cmd) {
    if (!b.shm || b.dead || !BridgeTakePending(INFINITE)) return false;
    b.shm->cmd = cmd;
    SetEvent(b.request);
    b.pending = true;
    return true;
}

void BridgeStop() {
    if (b.process) {
        if (!b.dead) BridgeCall(kBridgeShutdown, 5000);
        b.pending = false;
        if (WaitForSingleObject(b.process, 2000) != WAIT_OBJECT_0) TerminateProcess(b.process, 1);
        CloseHandle(b.process);
    }
    if (b.shm) UnmapViewOfFile(b.shm);
    for (HANDLE h : {b.mapping, b.request, b.reply})
        if (h) CloseHandle(h);
    b = {};
}

bool BridgeStart(LUID luid) {
    DWORD pid = GetCurrentProcessId();
    char name[64];
    snprintf(name, sizeof(name), XR_BRIDGE_MAPPING, pid);
    b.mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(BridgeShared), name);
    b.shm = b.mapping ? (BridgeShared*)MapViewOfFile(b.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(BridgeShared)) : nullptr;
    snprintf(name, sizeof(name), XR_BRIDGE_REQUEST, pid);
    b.request = CreateEventA(nullptr, FALSE, FALSE, name);
    snprintf(name, sizeof(name), XR_BRIDGE_REPLY, pid);
    b.reply = CreateEventA(nullptr, FALSE, FALSE, name);
    if (!b.shm || !b.request || !b.reply) {
        Log("XR: bridge shared memory setup failed (%lu)", GetLastError());
        BridgeStop();
        return false;
    }
    memset(b.shm, 0, sizeof(BridgeShared));
    b.shm->protocolVersion = kBridgeProtocolVersion;

    std::string exe = std::string(GameDir()) + XR_BRIDGE_EXE;
    char cmdLine[MAX_PATH + 32];
    snprintf(cmdLine, sizeof(cmdLine), "\"%s\" %lu", exe.c_str(), pid);
    STARTUPINFOA si = {sizeof(si)};
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA(exe.c_str(), cmdLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, GameDir(), &si, &pi)) {
        Log("XR: cannot start %s (%lu)", exe.c_str(), GetLastError());
        BridgeStop();
        return false;
    }
    CloseHandle(pi.hThread);
    b.process = pi.hProcess;
    Log("XR: started %s (pid %lu)", XR_BRIDGE_EXE, pi.dwProcessId);

    b.shm->luidLow = luid.LowPart;
    b.shm->luidHigh = luid.HighPart;
    // Generous timeout: SteamVR may have to start up first.
    if (!BridgeCall(kBridgeInit, 120000) || !b.shm->result || b.shm->hostProtocolVersion != kBridgeProtocolVersion) {
        if (b.shm->hostProtocolVersion != kBridgeProtocolVersion)
            Log("XR: bridge version mismatch; replace nfsmw_vr_host.exe with the one shipped with this VR mod");
        Log("XR: the bridge could not start OpenXR - see nfsmw_vr_host.log");
        BridgeStop();
        return false;
    }
    s.recW = b.shm->recW;
    s.recH = b.shm->recH;
    strncpy(s.runtimeName, b.shm->runtimeName, sizeof(s.runtimeName) - 1);
    Log("XR: runtime \"%s\" via the 64-bit bridge, per-eye %dx%d", s.runtimeName, s.recW, s.recH);
    return true;
}
#endif

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
static bool InitRuntime(LUID d3d9Luid) {
    if (!LoadRuntime()) {
        s.loadFailed = true;
        return false;
    }

    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool hasD3D11 = false;
    for (auto& e : exts) hasD3D11 |= strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME) == 0;
    if (!hasD3D11) {
        Log("XR: runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ici.applicationInfo.applicationName, "NFS Most Wanted VR");
    strcpy(ici.applicationInfo.engineName, "nfsmw-vr");
    ici.applicationInfo.applicationVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;
    XrResult r = xrCreateInstance(&ici, &s.instance);
    if (XR_FAILED(r)) {
        Log("XR: xrCreateInstance failed (%d)", (int)r);
        return false;
    }
    if (!LoadInstanceFunctions()) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(s.instance, &ip);
    strncpy(s.runtimeName, ip.runtimeName, sizeof(s.runtimeName) - 1);
    Log("XR: runtime \"%s\" %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(s.instance, &sgi, &s.system));

    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(xrGetD3D11GraphicsRequirementsKHR(s.instance, s.system, &req));
    if (req.adapterLuid.LowPart != d3d9Luid.LowPart || req.adapterLuid.HighPart != d3d9Luid.HighPart)
        Log("XR: WARNING - game renders on a different GPU than the headset; texture sharing will likely fail");
    if (!CreateD3D11(req.adapterLuid, req.minFeatureLevel)) return false;

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = s.dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s.system;
    XR_CHECK(xrCreateSession(s.instance, &sci, &s.session));

    XrReferenceSpaceCreateInfo rci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rci.poseInReferenceSpace.orientation.w = 1;
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(s.session, &rci, &s.local));
    rci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(s.session, &rci, &s.view));

    uint32_t vc = 0;
    xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
    std::vector<XrViewConfigurationView> views(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc, views.data());
    if (vc > 0) {
        s.recW = (int)views[0].recommendedImageRectWidth;
        s.recH = (int)views[0].recommendedImageRectHeight;
        Log("XR: recommended per-eye resolution %dx%d", s.recW, s.recH);
    }

    if (!PickFormat()) return false;
    Log("XR: initialised");
    return true;
}

bool Xr_Init(LUID d3d9Luid) {
#ifdef NFSMW_XR_HOST
    return InitRuntime(d3d9Luid);
#else
    bool bridge = WantBridge();
    if (!bridge && InitRuntime(d3d9Luid)) return true;
    if (bridge || s.loadFailed) {
        Log(bridge ? "XR: using the 64-bit bridge" : "XR: no usable 32-bit runtime, trying the 64-bit bridge");
        if (BridgeStart(d3d9Luid)) {
            s.remote = true;
            return true;
        }
    }
    if (!g_cfg.fakeHmd) return false;
    Log("XR: FakeHmd enabled - running the stereo path without a headset");
    if (s.session) xrDestroySession(s.session);
    s.session = XR_NULL_HANDLE;
    s.fake = true;
    s.recW = 1832; // typical Quest per-eye size, so Resolution=auto can be tested
    s.recH = 1920;
    FillFakeEyes();
    return true;
#endif
}

void Xr_Shutdown() {
#ifndef NFSMW_XR_HOST
    if (s.remote) {
        BridgeStop();
        s.remote = false;
        return;
    }
#endif
    for (auto& c : s.chains) DestroySwapchain(c);
    if (s.session) xrDestroySession(s.session);
    if (s.instance) xrDestroyInstance(s.instance);
    s.session = XR_NULL_HANDLE;
    s.instance = XR_NULL_HANDLE;
}

bool Xr_Available() { return s.fake || s.remote || s.session != XR_NULL_HANDLE; }
void Xr_RecommendedSize(int* width, int* height) {
    *width = s.recW;
    *height = s.recH;
}
bool Xr_FrameBegun() { return s.frameBegun; }

float Xr_DisplayHz() {
#ifndef NFSMW_XR_HOST
    if (s.remote) return b.shm ? b.shm->displayHz : 0.0f;
#endif
    XrDuration period = s.frameState.predictedDisplayPeriod;
    return period > 0 ? (float)(1e9 / (double)period) : 0.0f;
}
const EyeView& Xr_Eye(int eye) { return s.eyes[eye]; }
const char* Xr_RuntimeName() { return s.runtimeName; }

void Xr_PollEvents() {
    if (s.session) PollEvents();
}

void Xr_Recenter() {
#ifndef NFSMW_XR_HOST
    if (s.remote) {
        BridgeCall(kBridgeRecenter);
        return;
    }
#endif
    s.recenterPending = true;
}

// Frame timing at the runtime, logged every 5 s: how long xrWaitFrame blocked and xrEndFrame took,
// and how many display slots were skipped (the predicted display time moving on by more than one
// period: the headset showed a reprojected or repeated frame there).
struct XrTiming {
    LARGE_INTEGER freq{}, last{};
    double waitSum = 0, waitMax = 0, endSum = 0, endMax = 0;
    int frames = 0, skipped = 0, skippedMax = 0;
    XrTime lastDisplay = 0;
} t_xr;

double XrSeconds() {
    if (!t_xr.freq.QuadPart) QueryPerformanceFrequency(&t_xr.freq);
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return (double)n.QuadPart / (double)t_xr.freq.QuadPart;
}

void XrTimingFrame(double wait, XrTime display, XrDuration period) {
    t_xr.frames++;
    t_xr.waitSum += wait;
    if (wait > t_xr.waitMax) t_xr.waitMax = wait;
    if (t_xr.lastDisplay && period > 0) {
        long long slots = (long long)((display - t_xr.lastDisplay + period / 2) / period);
        if (slots > 1) {
            t_xr.skipped += (int)(slots - 1);
            if (slots - 1 > t_xr.skippedMax) t_xr.skippedMax = (int)(slots - 1);
        }
    }
    t_xr.lastDisplay = display;
    double now = XrSeconds();
    if (!t_xr.last.QuadPart) QueryPerformanceCounter(&t_xr.last);
    double since = now - (double)t_xr.last.QuadPart / (double)t_xr.freq.QuadPart;
    if (since < 5.0 || t_xr.frames == 0) return;
    XrTime keep = t_xr.lastDisplay;
    LARGE_INTEGER freq = t_xr.freq;
    t_xr = XrTiming();
    t_xr.freq = freq;
    t_xr.lastDisplay = keep;
    QueryPerformanceCounter(&t_xr.last);
}

bool Xr_BeginFrame() {
    if (s.frameBegun) return s.viewsValid;
    s.viewsValid = false;
    if (s.fake) {
        s.frameBegun = true;
        s.viewsValid = true;
        return true;
    }
#ifndef NFSMW_XR_HOST
    if (s.remote) {
        if (!BridgeCall(kBridgeBeginFrame)) return false;
        s.frameBegun = b.shm->frameBegun != 0;
        s.frameState.shouldRender = b.shm->shouldRender;
        s.eyes[0] = b.shm->eyes[0];
        s.eyes[1] = b.shm->eyes[1];
        s.viewsValid = b.shm->result != 0;
        return s.viewsValid;
    }
#endif
    if (!s.session) return false;
    PollEvents();
    if (!s.running) return false;

    s.frameState = {XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    double waitStart = XrSeconds();
    if (XR_FAILED(xrWaitFrame(s.session, &wi, &s.frameState))) return false;
    XrTimingFrame(XrSeconds() - waitStart, s.frameState.predictedDisplayTime, s.frameState.predictedDisplayPeriod);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(s.session, &bi))) return false;
    s.frameBegun = true;
    if (!s.frameState.shouldRender) return false;

    if (s.recenterPending || !s.haveOrigin) UpdateOrigin(s.frameState.predictedDisplayTime);

    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = s.frameState.predictedDisplayTime;
    li.space = s.local;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t count = 0;
    if (XR_FAILED(xrLocateViews(s.session, &li, &vs, 2, &count, views)) || count != 2 ||
        !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
        return false;

    Quat invOrigin = QConj(s.originRot);
    for (int i = 0; i < 2; i++) {
        EyeView& e = s.eyes[i];
        const XrPosef& p = views[i].pose;
        e.orientation = {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w};
        e.position = {p.position.x, p.position.y, p.position.z};
        e.relOrientation = QMul(invOrigin, e.orientation);
        e.relPosition = QRotate(invOrigin, e.position - s.originPos);
        e.tanLeft = tanf(-views[i].fov.angleLeft);
        e.tanRight = tanf(views[i].fov.angleRight);
        e.tanUp = tanf(views[i].fov.angleUp);
        e.tanDown = tanf(-views[i].fov.angleDown);
    }
    s.viewsValid = true;
    return true;
}

bool Xr_SetSharedTextures(HANDLE eye0, HANDLE eye1, HANDLE hud, int width, int height) {
    if (s.fake) return true;
#ifndef NFSMW_XR_HOST
    if (s.remote) {
        // D3D9Ex shared handles are global 32-bit values; the host sign-extends them back.
        b.shm->handles[0] = (uint32_t)(uintptr_t)eye0;
        b.shm->handles[1] = (uint32_t)(uintptr_t)eye1;
        b.shm->handles[2] = (uint32_t)(uintptr_t)hud;
        b.shm->width = width;
        b.shm->height = height;
        return BridgeCall(kBridgeSetTextures) && b.shm->result;
    }
#endif
    for (int i = 0; i < 3; i++) {
        if (s.eyeSrv[i]) s.eyeSrv[i]->Release();
        if (s.eyeTex[i]) s.eyeTex[i]->Release();
        s.eyeSrv[i] = nullptr;
        s.eyeTex[i] = nullptr;
        HANDLE h = i == 0 ? eye0 : i == 1 ? eye1 : hud;
        if (FAILED(s.dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void**)&s.eyeTex[i]))) {
            Log("XR: OpenSharedResource failed for eye %d", i);
            return false;
        }
        if (FAILED(s.dev->CreateShaderResourceView(s.eyeTex[i], nullptr, &s.eyeSrv[i]))) {
            Log("XR: CreateShaderResourceView failed for eye %d", i);
            return false;
        }
    }
    s.eyeW = width;
    s.eyeH = height;
    for (auto& c : s.chains)
        if (!CreateSwapchain(c, width, height)) return false;
    Log("XR: shared eye textures %dx%d attached", width, height);
    return true;
}

static bool BlitToSwapchain(Swapchain& sc, ID3D11ShaderResourceView* srv, bool keepAlpha = false) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(sc.handle, &ai, &index));
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(sc.handle, &wi));

    float params[4] = {s.formatSrgb ? 1.0f : 0.0f, keepAlpha ? 1.0f : 0.0f, 0, 0};
    s.ctx->UpdateSubresource(s.cbuf, 0, nullptr, params, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)sc.width, (float)sc.height, 0, 1};
    s.ctx->OMSetRenderTargets(1, &sc.rtvs[index], nullptr);
    s.ctx->RSSetViewports(1, &vp);
    s.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s.ctx->IASetInputLayout(nullptr);
    s.ctx->VSSetShader(s.vs, nullptr, 0);
    s.ctx->PSSetShader(s.ps, nullptr, 0);
    s.ctx->PSSetShaderResources(0, 1, &srv);
    s.ctx->PSSetSamplers(0, 1, &s.sampler);
    s.ctx->PSSetConstantBuffers(0, 1, &s.cbuf);
    s.ctx->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    s.ctx->PSSetShaderResources(0, 1, &nullSrv);
    s.ctx->OMSetRenderTargets(0, nullptr, nullptr);

    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(sc.handle, &ri));
    return true;
}

static bool ClearSwapchain(Swapchain& sc) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(sc.handle, &ai, &index));
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(sc.handle, &wi));
    const float black[4] = {0, 0, 0, 1};
    s.ctx->ClearRenderTargetView(sc.rtvs[index], black);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(sc.handle, &ri));
    return true;
}

// The frontend/HUD is authored for 4:3 and stretched to the back buffer, so show
// its panels at 4:3 whatever the render resolution is.
static const float kPanelAspect = 3.0f / 4.0f;

static XrPosef ToXr(Quat q, V3 p) {
    XrPosef r;
    r.orientation = {q.x, q.y, q.z, q.w};
    r.position = {p.x, p.y, p.z};
    return r;
}

void Xr_EndFrame(LayerMode mode, float tanX, float tanY, bool withHud) {
    if (!s.frameBegun) return;
    s.frameBegun = false;
    bool viewsValid = s.viewsValid;
    s.viewsValid = false;
    if (s.fake) return;
#ifndef NFSMW_XR_HOST
    if (s.remote) {
        b.shm->mode = (int32_t)mode;
        b.shm->tanX = tanX;
        b.shm->tanY = tanY;
        b.shm->withHud = withHud;
        b.shm->hudDistance = g_cfg.hudDistance;
        b.shm->hudWidth = g_cfg.hudWidth;
        b.shm->hudHeight = g_cfg.hudHeight;
        b.shm->hudRight = g_cfg.hudRight;
        b.shm->screenDistance = g_cfg.screenDistance;
        b.shm->screenWidth = g_cfg.screenWidth;
        if (g_cfg.asyncSubmit) BridgePost(kBridgeEndFrame);
        else BridgeCall(kBridgeEndFrame);
        return;
    }
#endif

    double endStart = XrSeconds();
    bool ready = viewsValid && s.frameState.shouldRender && s.eyeSrv[0] && s.chains[0].handle;
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                             {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[2];
    uint32_t layerCount = 0;

    if (ready && mode == LayerMode::Stereo) {
        bool ok = true;
        for (int i = 0; i < 2 && ok; i++) {
            ok = BlitToSwapchain(s.chains[i], s.eyeSrv[i]);
            pv[i].pose = ToXr(s.eyes[i].orientation, s.eyes[i].position);
            float ax = atanf(tanX), ay = atanf(tanY);
            pv[i].fov = {-ax, ax, ay, -ay};
            pv[i].subImage.swapchain = s.chains[i].handle;
            pv[i].subImage.imageRect = {{0, 0}, {s.chains[i].width, s.chains[i].height}};
        }
        if (ok) {
            proj.space = s.local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
        }
        if (ok && withHud && s.chains[2].handle && BlitToSwapchain(s.chains[2], s.eyeSrv[2], true)) {
            // Seat-locked transparent panel in front of the driver.
            V3 offset = QRotate(s.originRot, {g_cfg.hudRight, g_cfg.hudHeight, -g_cfg.hudDistance});
            hud.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                             XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            hud.space = s.local;
            hud.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            hud.subImage.swapchain = s.chains[2].handle;
            hud.subImage.imageRect = {{0, 0}, {s.chains[2].width, s.chains[2].height}};
            hud.pose = ToXr(s.originRot, s.originPos + offset);
            hud.size = {g_cfg.hudWidth, g_cfg.hudWidth * kPanelAspect};
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&hud;
        }
    } else if (ready && mode == LayerMode::Screen) {
        // Black surroundings behind the screen. SteamVR treats frames without a projection
        // layer as dropped and shows its own loading environment instead.
        if (ClearSwapchain(s.chains[1])) {
            for (int i = 0; i < 2; i++) {
                const EyeView& e = s.eyes[i];
                pv[i].pose = ToXr(e.orientation, e.position);
                pv[i].fov = {-atanf(e.tanLeft), atanf(e.tanRight), atanf(e.tanUp), -atanf(e.tanDown)};
                pv[i].subImage.swapchain = s.chains[1].handle;
                pv[i].subImage.imageRect = {{0, 0}, {s.chains[1].width, s.chains[1].height}};
            }
            proj.space = s.local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&proj;
        }
        if (BlitToSwapchain(s.chains[0], s.eyeSrv[0])) {
            V3 offset = QRotate(s.originRot, {0, 0, -g_cfg.screenDistance});
            quad.space = s.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = s.chains[0].handle;
            quad.subImage.imageRect = {{0, 0}, {s.chains[0].width, s.chains[0].height}};
            quad.pose = ToXr(s.originRot, s.originPos + offset);
            quad.size = {g_cfg.screenWidth, g_cfg.screenWidth * kPanelAspect};
            layers[layerCount++] = (XrCompositionLayerBaseHeader*)&quad;
        }
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = s.frameState.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    XrResult r = xrEndFrame(s.session, &ei);
    if (XR_FAILED(r)) Log("XR: xrEndFrame failed: %s", ResultStr(r));
    double took = XrSeconds() - endStart;
    t_xr.endSum += took;
    if (took > t_xr.endMax) t_xr.endMax = took;
}
