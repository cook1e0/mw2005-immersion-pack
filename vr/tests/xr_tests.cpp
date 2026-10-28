// Fake OpenXR entry points exercise the production frame lifecycle without a runtime.
#define NFSMW_XR_HOST
#include "../src/xr.cpp"
#include <assert.h>
Config g_cfg;
void Log(const char*, ...) {}
static bool render = true, locate = false;
static XrViewStateFlags flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
static unsigned waitCalls, endCalls, submitted;
static XrResult XRAPI_CALL Poll(XrInstance, XrEventDataBuffer*) { return XR_EVENT_UNAVAILABLE; }
static XrResult XRAPI_CALL Wait(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
    ++waitCalls; state->shouldRender = render; state->predictedDisplayTime = 1; return XR_SUCCESS;
}
static XrResult XRAPI_CALL Begin(XrSession, const XrFrameBeginInfo*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL Space(XrSpace, XrSpace, XrTime, XrSpaceLocation*) { return XR_ERROR_RUNTIME_FAILURE; }
static XrResult XRAPI_CALL Views(XrSession, const XrViewLocateInfo*, XrViewState* state, uint32_t, uint32_t* count, XrView* views) {
    if (!locate) return XR_ERROR_RUNTIME_FAILURE;
    *count = 2; state->viewStateFlags = flags;
    for (int i = 0; i < 2; ++i) {
        views[i].pose.orientation.w = 1;
        views[i].pose.position.x = i ? 0.032f : -0.032f;
        views[i].fov = {-0.8f, 0.8f, 0.8f, -0.8f};
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL End(XrSession, const XrFrameEndInfo* info) {
    ++endCalls; submitted = info->layerCount; return XR_SUCCESS;
}
// Two fake API layers in front of a fake runtime, each passing the call down like a real layer.
static std::string chain;
static XrResult XRAPI_CALL RuntimeCreate(const XrInstanceCreateInfo*, XrInstance* instance) {
    chain += "runtime";
    *instance = (XrInstance)7;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL RuntimeGipa(XrInstance, const char*, PFN_xrVoidFunction*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL LayerGipa(XrInstance, const char*, PFN_xrVoidFunction*) { return XR_SUCCESS; }
static XrResult XRAPI_CALL LayerCreate(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* li, XrInstance* instance) {
    const XrApiLayerNextInfo* ni = li->nextInfo;
    chain += std::string(ni->layerName) + ">";
    assert(ni->structType == XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO);
    assert(strcmp(ni->layerName, "A") == 0 ? ni->nextGetInstanceProcAddr == LayerGipa
                                           : ni->nextGetInstanceProcAddr == RuntimeGipa && !ni->next);
    XrApiLayerCreateInfo down = *li;
    down.nextInfo = ni->next;
    return ni->nextCreateApiLayerInstance(info, &down, instance);
}

static void TestLayerChain() {
    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    XrInstance inst = XR_NULL_HANDLE;
    xrCreateInstance = RuntimeCreate;
    s_gipa = RuntimeGipa;
    assert(XR_SUCCEEDED(CreateInstance(&ici, &inst)) && chain == "runtime" && s_gipa == RuntimeGipa);
    chain.clear();
    s_layers = {{"A", LayerGipa, LayerCreate}, {"B", LayerGipa, LayerCreate}};
    assert(XR_SUCCEEDED(CreateInstance(&ici, &inst)) && inst == (XrInstance)7);
    assert(chain == "A>B>runtime" && s_gipa == LayerGipa);
    s_layers.clear();

    std::string m = R"({ "api_layer": { "name": "XR_APILAYER_test", "library_path": ".\\x.dll",
        "instance_extensions": [{ "name": "XR_EXT_x" }], "disable_environment": "DISABLE_X" } })";
    assert(LayerName(m) == "XR_APILAYER_test" && JsonString(m, "disable_environment") == "DISABLE_X");
    assert(ReadLibraryPath("C:\\L\\x.json", m) == "C:\\L\\.\\x.dll" && JsonString(m, "enable_environment").empty());
}

int main() {
    TestLayerChain();
    s.session = (XrSession)1; s.running = true;
    xrPollEvent = Poll; xrWaitFrame = Wait; xrBeginFrame = Begin;
    xrLocateSpace = Space; xrLocateViews = Views; xrEndFrame = End;
    unsigned initialEpoch = Xr_PoseEpoch();
    assert(!Xr_BeginFrame() && !Xr_BeginFrame() && Xr_FrameBegun() && waitCalls == 1);
    assert(Xr_PoseEpoch() != initialEpoch);
    // Even with existing textures, a failed pose lookup must submit no layers.
    s.eyeSrv[0] = (ID3D11ShaderResourceView*)1; s.chains[0].handle = (XrSwapchain)1;
    Xr_EndFrame(LayerMode::Stereo, 1, 1, true);
    assert(!Xr_FrameBegun() && !s.viewsValid && endCalls == 1 && submitted == 0);
    s.eyeSrv[0] = nullptr; s.chains[0].handle = XR_NULL_HANDLE;
    locate = true; flags = 0;
    assert(!Xr_BeginFrame() && !Xr_BeginFrame());
    Xr_EndFrame(LayerMode::None, 0, 0, false);
    flags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    assert(Xr_BeginFrame() && Xr_BeginFrame());
    assert(waitCalls == 3 && s.eyes[0].tanLeft > 0);
    Quat renderedPose = Xr_Eye(0).relOrientation;
    Xr_EndFrame(LayerMode::None, 0, 0, false);
    assert(!memcmp(&renderedPose, &Xr_Eye(0).relOrientation, sizeof(renderedPose)));
    unsigned beforeRecenter = Xr_PoseEpoch();
    Xr_Recenter();
    assert(Xr_PoseEpoch() != beforeRecenter);
    render = false;
    assert(!Xr_BeginFrame() && !Xr_BeginFrame() && Xr_FrameBegun());
    Xr_EndFrame(LayerMode::None, 0, 0, false);
    assert(waitCalls == 4 && endCalls == 4);
    s.fake = true;
    assert(Xr_BeginFrame() && Xr_BeginFrame());
    Xr_EndFrame(LayerMode::None, 0, 0, false);
    assert(!Xr_FrameBegun() && !s.viewsValid && endCalls == 4);
    puts("XR frame tests passed");
}
