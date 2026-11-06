#include "xr_app.hpp"

#include <android_native_app_glue.h>

#include <cstring>
#include <string>

#include "kage/util/log.hpp"

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
    if (display_ == EGL_NO_DISPLAY || !eglInitialize(display_, nullptr, nullptr)) {
        klog::error("EGL: no display");
        return false;
    }
    const EGLint cfgAttribs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint n = 0;
    if (!eglChooseConfig(display_, cfgAttribs, &config_, 1, &n) || n < 1) {
        klog::error("EGL: no RGBA8 ES3 config");
        return false;
    }
    const EGLint ctxAttribs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    context_ = eglCreateContext(display_, config_, EGL_NO_CONTEXT, ctxAttribs);
    const EGLint pbAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    surface_ = eglCreatePbufferSurface(display_, config_, pbAttribs);
    if (context_ == EGL_NO_CONTEXT || surface_ == EGL_NO_SURFACE || !eglMakeCurrent(display_, surface_, surface_, context_)) {
        klog::error("EGL: context creation failed (0x{:x})", eglGetError());
        return false;
    }
    klog::info("GL: {} / {}", reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
              reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
}

bool XrApp::init(android_app* app) {
    auto initLoader = getProc<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
    if (!initLoader) {
        klog::error("xrInitializeLoaderKHR unavailable");
        return false;
    }
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
        if (std::strcmp(e.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
            hasRefreshRateExt_ = true;
            enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        }
    }
    klog::info("OpenXR runtime exposes {} instance extensions", extCount);

    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = app->activity->vm;
    ai.applicationActivity = app->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.enabledExtensionNames = enable.data();
    std::strncpy(ci.applicationInfo.applicationName, "KAGE VR Stream", XR_MAX_APPLICATION_NAME_SIZE - 1);
    std::strncpy(ci.applicationInfo.engineName, "KAGE", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    if (!xrOk(XR_NULL_HANDLE, xrCreateInstance(&ci, &instance_), "xrCreateInstance")) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(instance_, &ip);
    klog::info("OpenXR runtime: {} {}.{}.{}", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
              XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!xrOk(instance_, xrGetSystem(instance_, &sgi, &systemId_), "xrGetSystem")) return false;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(instance_, systemId_, &sp);
    klog::info("XR system: {} (vendor 0x{:x}), max swapchain {}x{}", sp.systemName, sp.vendorId,
              sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);

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
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!xrOk(instance_, xrCreateReferenceSpace(session_, &rs, &localSpace_), "local space")) return false;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xrOk(instance_, xrCreateReferenceSpace(session_, &rs, &viewSpace_), "view space")) return false;

    if (!createSwapchains()) return false;
    logRefreshRates();
    return true;
}

bool XrApp::createSwapchains() {
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    if (n != 2) {
        klog::error("expected 2 stereo views, runtime reports {}", n);
        return false;
    }
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
    if (!format) {
        klog::error("no supported RGBA8 swapchain format");
        return false;
    }

    for (size_t i = 0; i < 2; ++i) {
        auto& eye = eyes_[i];
        eye.width = static_cast<int32_t>(views[i].recommendedImageRectWidth);
        eye.height = static_cast<int32_t>(views[i].recommendedImageRectHeight);
        klog::info("eye {}: recommended {}x{} (max {}x{}), samples {}", i, eye.width, eye.height,
                  views[i].maxImageRectWidth, views[i].maxImageRectHeight, views[i].recommendedSwapchainSampleCount);
        XrSwapchainCreateInfo sc{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sc.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
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
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                klog::error("framebuffer incomplete for eye {} image {}", i, k);
                return false;
            }
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    return true;
}

void XrApp::logRefreshRates() {
    if (!hasRefreshRateExt_) {
        klog::info("refresh rates: Non disponible (XR_FB_display_refresh_rate absent)");
        return;
    }
    auto enumRates = getProc<PFN_xrEnumerateDisplayRefreshRatesFB>(instance_, "xrEnumerateDisplayRefreshRatesFB");
    auto getRate = getProc<PFN_xrGetDisplayRefreshRateFB>(instance_, "xrGetDisplayRefreshRateFB");
    if (!enumRates || !getRate) return;
    uint32_t n = 0;
    enumRates(session_, 0, &n, nullptr);
    std::vector<float> rates(n);
    enumRates(session_, n, &n, rates.data());
    std::string s;
    for (float r : rates) s += std::to_string(static_cast<int>(r + 0.5f)) + " ";
    float cur = 0;
    getRate(session_, &cur);
    klog::info("refresh rates supported: {}Hz, current {:.1f} Hz", s, cur);
}

void XrApp::pollEvents(bool& exitRequested) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(instance_, &ev) == XR_SUCCESS) {
        switch (ev.type) {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                exitRequested = true;
                break;
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
                state_ = sc->state;
                klog::info("session state -> {}", static_cast<int>(state_));
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
                break;
            }
            default:
                break;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

void XrApp::renderFrame(const std::array<float, 4>& c) {
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(session_, &wi, &fs))) return;
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(session_, &bi))) return;

    std::array<XrCompositionLayerProjectionView, 2> pv{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    const XrCompositionLayerBaseHeader* layers[] = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    uint32_t layerCount = 0;

    if (fs.shouldRender) {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = localSpace_;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
        uint32_t vc = 0;
        if (XR_SUCCEEDED(xrLocateViews(session_, &vli, &vs, 2, &vc, views.data())) && vc == 2 &&
            (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            for (size_t i = 0; i < 2; ++i) {
                auto& eye = eyes_[i];
                uint32_t idx = 0;
                XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                xrAcquireSwapchainImage(eye.swapchain, &ai, &idx);
                XrSwapchainImageWaitInfo wsi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                wsi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(eye.swapchain, &wsi);
                glBindFramebuffer(GL_FRAMEBUFFER, eye.framebuffers[idx]);
                glViewport(0, 0, eye.width, eye.height);
                glClearColor(c[0], c[1], c[2], c[3]);
                glClear(GL_COLOR_BUFFER_BIT);
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                xrReleaseSwapchainImage(eye.swapchain, &ri);

                pv[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[i].pose = views[i].pose;
                pv[i].fov = views[i].fov;
                pv[i].subImage.swapchain = eye.swapchain;
                pv[i].subImage.imageRect = {{0, 0}, {eye.width, eye.height}};
                pv[i].subImage.imageArrayIndex = 0;
            }
            layer.space = localSpace_;
            layer.viewCount = 2;
            layer.views = pv.data();
            layerCount = 1;
        }
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(viewSpace_, localSpace_, fs.predictedDisplayTime, &loc))) {
            headValid_ = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
                         (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
            if (headValid_) headPose_ = loc.pose;
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
        if (eye.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(eye.swapchain);
        eye = {};
    }
    if (viewSpace_ != XR_NULL_HANDLE) xrDestroySpace(viewSpace_);
    if (localSpace_ != XR_NULL_HANDLE) xrDestroySpace(localSpace_);
    if (session_ != XR_NULL_HANDLE) xrDestroySession(session_);
    if (instance_ != XR_NULL_HANDLE) xrDestroyInstance(instance_);
    viewSpace_ = localSpace_ = XR_NULL_HANDLE;
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
