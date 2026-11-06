#include "xr_app.hpp"

#include <android_native_app_glue.h>

#include <cstring>
#include <ctime>
#include <string>

#include "kage/util/log.hpp"
#include "xr_math.hpp"

namespace klog = kage::log;

namespace {

bool xrOk(XrInstance inst, XrResult r, const char* what) {
    if (XR_SUCCEEDED(r)) return true;
    char name[XR_MAX_RESULT_STRING_SIZE] = "?";
    if (inst != XR_NULL_HANDLE) xrResultToString(inst, r, name);
    klog::error("OpenXR {} failed: {} ({})", what, name, static_cast<int>(r));
    return false;
}

template <class Fn>
Fn getProc(XrInstance inst, const char* name) {
    PFN_xrVoidFunction f = nullptr;
    if (XR_FAILED(xrGetInstanceProcAddr(inst, name, &f))) return nullptr;
    return reinterpret_cast<Fn>(f);
}

constexpr int64_t kGlSrgb8Alpha8 = 0x8C43;
constexpr int64_t kGlRgba8 = 0x8058;

}  // namespace

bool XrApp::initEgl() {
    display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) return false;
    const EGLint cfgAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(display_, cfgAttribs, &config_, 1, &n) || n < 1) return false;
    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config_, pb);
    if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE || !eglMakeCurrent(display_, surface_, surface_, context_)) {
        klog::error("EGL: context creation failed (0x{:x})", eglGetError());
        return false;
    }
    klog::info("GL: {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    return true;
}

bool XrApp::init(android_app* app) {
    auto initLoader = getProc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
    if (!initLoader) return false;
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = app->activity->vm;
    li.applicationContext = app->activity->clazz;
    if (!xrOk(XR_NULL_HANDLE, initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&li)), "loader init"))
        return false;

    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    std::vector<const char*> enable = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    for (const auto& e : exts) {
        if (!std::strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) {
            hasRefreshExt_ = true;
            enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        } else if (!std::strcmp(e.extensionName, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) {
            hasHandExt_ = true;
            enable.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME);
        } else if (!std::strcmp(e.extensionName, XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME)) {
            hasTimespec_ = true;
            enable.push_back(XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME);
        }
    }

    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = app->activity->vm;
    ai.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.enabledExtensionNames = enable.data();
    std::strncpy(ci.applicationInfo.applicationName, "KAGE VR Stream", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ci.applicationInfo.engineName, "KAGE", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 2;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (!xrOk(XR_NULL_HANDLE, xrCreateInstance(&ci, &instance_), "xrCreateInstance")) return false;

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrOk(instance_, xrGetSystem(instance_, &sgi, &systemId_), "xrGetSystem")) return false;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, systemId_, &sp);
    klog::info("XR system: {}, hand tracking ext: {}", sp.systemName, hasHandExt_);

    auto getReq = getProc<PFN_xrGetOpenGLESGraphicsRequirementsKHR>(instance_, "xrGetOpenGLESGraphicsRequirementsKHR");
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!getReq || !xrOk(instance_, getReq(instance_, systemId_, &req), "GLES requirements")) return false;
    if (!initEgl()) return false;

    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = display_;
    gb.config = config_;
    gb.context = context_;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &gb;
    si.systemId = systemId_;
    if (!xrOk(instance_, xrCreateSession(instance_, &si, &session_), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;  // floor at y = 0, what SteamVR expects
    if (XR_FAILED(xrCreateReferenceSpace(session_, &rs, &stage_))) {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!xrOk(instance_, xrCreateReferenceSpace(session_, &rs, &stage_), "local space")) return false;
        klog::warn("STAGE space unavailable, using LOCAL");
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xrOk(instance_, xrCreateReferenceSpace(session_, &rs, &view_), "view space")) return false;

    if (!createSwapchains()) return false;
    initInput();
    initHands();

    if (hasRefreshExt_) {
        auto enumRates = getProc<PFN_xrEnumerateDisplayRefreshRatesFB>(instance_, "xrEnumerateDisplayRefreshRatesFB");
        uint32_t n = 0;
        if (enumRates && XR_SUCCEEDED(enumRates(session_, 0, &n, nullptr))) {
            rates_.resize(n);
            enumRates(session_, n, &n, rates_.data());
        }
    }
    return true;
}

bool XrApp::createSwapchains() {
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    if (n != 2) return false;
    std::vector<XrViewConfigurationView> views(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, views.data());
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(session_, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(session_, fc, &fc, formats.data());
    int64_t format = 0;
    for (int64_t want : {kGlSrgb8Alpha8, kGlRgba8})
        for (int64_t f : formats)
            if (!format && f == want) format = f;
    if (!format) return false;

    for (size_t i = 0; i < 2; ++i) {
        auto& eye = eyes_[i];
        eye.width = static_cast<int32_t>(views[i].recommendedImageRectWidth);
        eye.height = static_cast<int32_t>(views[i].recommendedImageRectHeight);
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        sc.format = format;
        sc.sampleCount = 1;
        sc.width = static_cast<uint32_t>(eye.width);
        sc.height = static_cast<uint32_t>(eye.height);
        sc.faceCount = 1;
        sc.arraySize = 1;
        sc.mipCount = 1;
        if (!xrOk(instance_, xrCreateSwapchain(session_, &sc, &eye.swapchain), "xrCreateSwapchain")) return false;
        uint32_t ic = 0;
        xrEnumerateSwapchainImages(eye.swapchain, 0, &ic, nullptr);
        eye.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(eye.swapchain, ic, &ic, reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
        eye.framebuffers.assign(ic, 0);
        glGenFramebuffers(static_cast<GLsizei>(ic), eye.framebuffers.data());
        for (uint32_t k = 0; k < ic; ++k) {
            glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[k]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye.images[k].image, 0);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    klog::info("swapchains {}x{} per eye", eyes_[0].width, eyes_[0].height);
    return true;
}

void XrApp::initInput() {
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asi.actionSetName, "kage");
    std::strcpy(asi.localizedActionSetName, "KAGE");
    if (!xrOk(instance_, xrCreateActionSet(instance_, &asi, &actionSet_), "action set")) return;
    xrStringToPath(instance_, "/user/hand/left", &handPaths_[0]);
    xrStringToPath(instance_, "/user/hand/right", &handPaths_[1]);
    auto mk = [&](XrAction& a, const char* name, XrActionType type) {
        XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(ci.actionName, name);
        std::strcpy(ci.localizedActionName, name);
        ci.actionType = type;
        ci.countSubactionPaths = 2;
        ci.subactionPaths = handPaths_;
        xrCreateAction(actionSet_, &ci, &a);
    };
    mk(aGrip_, "grip", XR_ACTION_TYPE_POSE_INPUT);
    mk(aAim_, "aim", XR_ACTION_TYPE_POSE_INPUT);
    mk(aTrigger_, "trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    mk(aSqueeze_, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    mk(aStick_, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    mk(aStickClick_, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aStickTouch_, "stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aTriggerTouch_, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aBtn1_, "btn1", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aBtn1Touch_, "btn1_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aBtn2_, "btn2", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aBtn2Touch_, "btn2_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aMenu_, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aThumbrest_, "thumbrest", XR_ACTION_TYPE_BOOLEAN_INPUT);
    mk(aHaptic_, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT);

    std::vector<XrActionSuggestedBinding> b;
    auto bind = [&](XrAction a, const char* path) {
        XrPath p;
        if (XR_SUCCEEDED(xrStringToPath(instance_, path, &p))) b.push_back({a, p});
    };
    for (const char* h : {"left", "right"}) {
        const std::string s = std::string("/user/hand/") + h;
        const bool left = std::string(h) == "left";
        bind(aGrip_, (s + "/input/grip/pose").c_str());
        bind(aAim_, (s + "/input/aim/pose").c_str());
        bind(aTrigger_, (s + "/input/trigger/value").c_str());
        bind(aTriggerTouch_, (s + "/input/trigger/touch").c_str());
        bind(aSqueeze_, (s + "/input/squeeze/value").c_str());
        bind(aStick_, (s + "/input/thumbstick").c_str());
        bind(aStickClick_, (s + "/input/thumbstick/click").c_str());
        bind(aStickTouch_, (s + "/input/thumbstick/touch").c_str());
        bind(aThumbrest_, (s + "/input/thumbrest/touch").c_str());
        bind(aBtn1_, (s + (left ? "/input/x/click" : "/input/a/click")).c_str());
        bind(aBtn1Touch_, (s + (left ? "/input/x/touch" : "/input/a/touch")).c_str());
        bind(aBtn2_, (s + (left ? "/input/y/click" : "/input/b/click")).c_str());
        bind(aBtn2Touch_, (s + (left ? "/input/y/touch" : "/input/b/touch")).c_str());
        if (left) bind(aMenu_, "/user/hand/left/input/menu/click");
        bind(aHaptic_, (s + "/output/haptic").c_str());
    }
    XrPath profile;
    xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &profile);
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = profile;
    sb.suggestedBindings = b.data();
    sb.countSuggestedBindings = static_cast<uint32_t>(b.size());
    xrOk(instance_, xrSuggestInteractionProfileBindings(instance_, &sb), "suggest bindings");

    for (int i = 0; i < 2; ++i) {
        XrActionSpaceCreateInfo sci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sci.poseInActionSpace.orientation.w = 1;
        sci.subactionPath = handPaths_[i];
        sci.action = aGrip_;
        xrCreateActionSpace(session_, &sci, &gripSpace_[i]);
        sci.action = aAim_;
        xrCreateActionSpace(session_, &sci, &aimSpace_[i]);
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &actionSet_;
    xrOk(instance_, xrAttachSessionActionSets(session_, &at), "attach action sets");
}

void XrApp::initHands() {
    if (!hasHandExt_) return;
    auto create = getProc<PFN_xrCreateHandTrackerEXT>(instance_, "xrCreateHandTrackerEXT");
    locateJoints_ = getProc<PFN_xrLocateHandJointsEXT>(instance_, "xrLocateHandJointsEXT");
    if (!create || !locateJoints_) return;
    for (int i = 0; i < 2; ++i) {
        XrHandTrackerCreateInfoEXT ci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
        ci.hand = i == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
        ci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
        if (XR_FAILED(create(session_, &ci, &handTracker_[i]))) handTracker_[i] = XR_NULL_HANDLE;
    }
}

XrTime XrApp::now() const {
    if (!hasTimespec_) return 0;
    static auto conv = getProc<PFN_xrConvertTimespecTimeToTimeKHR>(instance_, "xrConvertTimespecTimeToTimeKHR");
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    XrTime t = 0;
    if (conv) conv(instance_, &ts, &t);
    return t;
}

bool XrApp::locateHead(XrTime t, XrPosef& out) const {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(view_, stage_, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    out = loc.pose;
    return true;
}

bool XrApp::locateController(int hand, XrTime t, XrPosef& out) const {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (!gripSpace_[hand] || XR_FAILED(xrLocateSpace(gripSpace_[hand], stage_, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    out = loc.pose;
    return true;
}

void XrApp::syncInput(XrTime t) {
    if (!actionSet_) return;
    XrActiveActionSet aas{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(session_, &sync))) return;
    for (int i = 0; i < 2; ++i) {
        auto& c = ctrl_[i];
        XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
        gi.subactionPath = handPaths_[i];
        auto f = [&](XrAction a) {
            XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
            gi.action = a;
            xrGetActionStateFloat(session_, &gi, &s);
            return s.isActive ? s.currentState : 0.f;
        };
        auto bl = [&](XrAction a) {
            XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
            gi.action = a;
            xrGetActionStateBoolean(session_, &gi, &s);
            return s.isActive && s.currentState;
        };
        c.trigger = f(aTrigger_);
        c.squeeze = f(aSqueeze_);
        XrActionStateVector2f v{XR_TYPE_ACTION_STATE_VECTOR2F};
        gi.action = aStick_;
        xrGetActionStateVector2f(session_, &gi, &v);
        c.stickX = v.isActive ? v.currentState.x : 0;
        c.stickY = v.isActive ? v.currentState.y : 0;
        c.stickClick = bl(aStickClick_);
        c.stickTouch = bl(aStickTouch_);
        c.triggerTouch = bl(aTriggerTouch_);
        c.btn1 = bl(aBtn1_);
        c.btn1Touch = bl(aBtn1Touch_);
        c.btn2 = bl(aBtn2_);
        c.btn2Touch = bl(aBtn2Touch_);
        c.menu = i == 0 && bl(aMenu_);
        c.thumbrest = bl(aThumbrest_);
        XrActionStatePose ps{XR_TYPE_ACTION_STATE_POSE};
        gi.action = aGrip_;
        xrGetActionStatePose(session_, &gi, &ps);
        XrSpaceLocation g{XR_TYPE_SPACE_LOCATION}, a{XR_TYPE_SPACE_LOCATION};
        c.active = ps.isActive && XR_SUCCEEDED(xrLocateSpace(gripSpace_[i], stage_, t, &g)) &&
                   (g.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (c.active) {
            c.grip = g.pose;
            if (XR_SUCCEEDED(xrLocateSpace(aimSpace_[i], stage_, t, &a))) c.aim = a.pose;
        }
    }
    for (int i = 0; i < 2; ++i) {
        auto& h = hands_[i];
        h.active = false;
        if (!handTracker_[i] || !locateJoints_) continue;
        XrHandJointLocationEXT j[XR_HAND_JOINT_COUNT_EXT];
        XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
        locs.jointCount = XR_HAND_JOINT_COUNT_EXT;
        locs.jointLocations = j;
        XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
        li.baseSpace = stage_;
        li.time = t;
        if (XR_FAILED(locateJoints_(handTracker_[i], &li, &locs)) || !locs.isActive) continue;
        h.active = true;
        for (int k = 0; k < XR_HAND_JOINT_COUNT_EXT; ++k) {
            h.joints[k] = j[k].pose;
            h.radii[k] = j[k].radius;
        }
    }
}

void XrApp::vibrate(int hand, float amplitude, float frequency, int64_t durationNs) {
    if (!aHaptic_) return;
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amplitude;
    v.frequency = frequency > 0 ? frequency : XR_FREQUENCY_UNSPECIFIED;
    v.duration = durationNs > 0 ? durationNs : XR_MIN_HAPTIC_DURATION;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = aHaptic_;
    hi.subactionPath = handPaths_[hand];
    xrApplyHapticFeedback(session_, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
}

float XrApp::refreshRate() const {
    if (!hasRefreshExt_) return 72.f;
    static auto get = getProc<PFN_xrGetDisplayRefreshRateFB>(instance_, "xrGetDisplayRefreshRateFB");
    float r = 72.f;
    if (get) get(session_, &r);
    return r;
}

void XrApp::setRefreshRate(float hz) {
    if (!hasRefreshExt_) return;
    auto req = getProc<PFN_xrRequestDisplayRefreshRateFB>(instance_, "xrRequestDisplayRefreshRateFB");
    if (req && XR_SUCCEEDED(req(session_, hz))) klog::info("refresh rate requested: {:.0f} Hz", hz);
}

bool XrApp::eyeFovAndIpd(std::array<XrFovf, 2>& fov, float& ipd) const {
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = now();
    vli.space = view_;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    std::array<XrView, 2> v{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    uint32_t n = 0;
    if (XR_FAILED(xrLocateViews(session_, &vli, &vs, 2, &n, v.data())) || n != 2) return false;
    fov = {v[0].fov, v[1].fov};
    ipd = xm::length(xm::sub(v[1].pose.position, v[0].pose.position));
    return true;
}

void XrApp::pollEvents(bool& exitRequested) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) exitRequested = true;
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            state_ = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev)->state;
            if (state_ == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                running_ = xrOk(instance_, xrBeginSession(session_, &bi), "xrBeginSession");
            } else if (state_ == XR_SESSION_STATE_STOPPING) {
                xrEndSession(session_);
                running_ = false;
            } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                exitRequested = true;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void XrApp::frame(const std::function<void(FrameContext&)>& prepare, const std::function<void(int, const FrameContext&)>& draw) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, &wi, &fs))) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(session_, &bi))) return;
    syncInput(fs.predictedDisplayTime);

    std::array<XrCompositionLayerProjectionView, 2> pv{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    uint32_t layerCount = 0;

    if (fs.shouldRender) {
        FrameContext ctx;
        ctx.displayTime = fs.predictedDisplayTime;
        ctx.width = eyes_[0].width;
        ctx.height = eyes_[0].height;
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = stage_;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        ctx.views = {{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        uint32_t vc = 0;
        const bool ok = XR_SUCCEEDED(xrLocateViews(session_, &vli, &vs, 2, &vc, ctx.views.data())) && vc == 2 &&
                        (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
        if (ok) {
            ctx.headValid = locateHead(fs.predictedDisplayTime, ctx.head);
            const XrPosef invHead = xm::inverse(ctx.head);
            for (int i = 0; i < 2; ++i) {
                ctx.eyeInHead[i] = xm::compose(invHead, ctx.views[i].pose);
                ctx.submitPose[i] = ctx.views[i].pose;
                ctx.submitFov[i] = ctx.views[i].fov;
            }
            prepare(ctx);
            for (int i = 0; i < 2; ++i) {
                auto& eye = eyes_[i];
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(eye.swapchain, &ai, &idx);
                XrSwapchainImageWaitInfo wsi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wsi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(eye.swapchain, &wsi);
                glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[idx]);
                glViewport(0, 0, eye.width, eye.height);
                draw(i, ctx);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(eye.swapchain, &ri);
                pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[i].pose = ctx.submitPose[i];
                pv[i].fov = ctx.submitFov[i];
                pv[i].subImage.swapchain = eye.swapchain;
                pv[i].subImage.imageRect = {{0, 0}, {eye.width, eye.height}};
            }
            layer.space = stage_;
            layer.viewCount = 2;
            layer.views = pv.data();
            layerCount = 1;
        }
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = layerCount;
    ei.layers = layers;
    xrEndFrame(session_, &ei);
}

void XrApp::shutdown() {
    for (auto& eye : eyes_) {
        if (!eye.framebuffers.empty()) glDeleteFramebuffers(static_cast<GLsizei>(eye.framebuffers.size()), eye.framebuffers.data());
        if (eye.swapchain) xrDestroySwapchain(eye.swapchain);
        eye = {};
    }
    if (instance_ && hasHandExt_) {
        auto destroy = getProc<PFN_xrDestroyHandTrackerEXT>(instance_, "xrDestroyHandTrackerEXT");
        for (auto& h : handTracker_)
            if (h && destroy) destroy(h);
    }
    if (actionSet_) xrDestroyActionSet(actionSet_);
    if (view_) xrDestroySpace(view_);
    if (stage_) xrDestroySpace(stage_);
    if (session_) xrDestroySession(session_);
    if (instance_) xrDestroyInstance(instance_);
    actionSet_ = XR_NULL_HANDLE;
    view_ = stage_ = XR_NULL_HANDLE;
    session_ = XR_NULL_HANDLE;
    instance_ = XR_NULL_HANDLE;
    if (display_ != EGL_NO_DISPLAY) {
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface_ != EGL_NO_SURFACE) eglDestroySurface(display_, surface_);
        if (context_ != EGL_NO_CONTEXT) eglDestroyContext(display_, context_);
        eglTerminate(display_);
    }
    display_ = EGL_NO_DISPLAY;
}
