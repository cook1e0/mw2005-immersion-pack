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
int main() {
    s.session = (XrSession)1; s.running = true;
    xrPollEvent = Poll; xrWaitFrame = Wait; xrBeginFrame = Begin;
    xrLocateSpace = Space; xrLocateViews = Views; xrEndFrame = End;
    assert(!Xr_BeginFrame() && !Xr_BeginFrame() && Xr_FrameBegun() && waitCalls == 1);
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
    Xr_EndFrame(LayerMode::None, 0, 0, false);
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
