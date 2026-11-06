// KAGE VR Stream Quest client - OpenXR (OpenGL ES) runtime wrapper:
// session, stereo swapchains, STAGE space, Touch controller actions, hand
// tracking (XR_EXT_hand_tracking), haptics and refresh-rate control.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_TIMESPEC
#include <time.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

struct android_app;

struct ControllerInput {
    bool active = false;  // controller tracked & connected
    XrPosef grip{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef aim{{0, 0, 0, 1}, {0, 0, 0}};
    float trigger = 0, squeeze = 0, stickX = 0, stickY = 0;
    bool btn1 = false, btn1Touch = false, btn2 = false, btn2Touch = false;  // A/B (right), X/Y (left)
    bool menu = false, stickClick = false, stickTouch = false, triggerTouch = false, thumbrest = false;
};

struct HandJoints {
    bool active = false;
    std::array<XrPosef, XR_HAND_JOINT_COUNT_EXT> joints{};
    std::array<float, XR_HAND_JOINT_COUNT_EXT> radii{};
};

struct FrameContext {
    XrTime displayTime = 0;
    XrPosef head{{0, 0, 0, 1}, {0, 0, 0}};
    bool headValid = false;
    std::array<XrView, 2> views{};          // eyes in STAGE space at displayTime
    std::array<XrPosef, 2> eyeInHead{};     // eye offsets in VIEW space
    std::array<XrPosef, 2> submitPose{};    // defaults to views[i].pose; overridden when streaming
    std::array<XrFovf, 2> submitFov{};
    int width = 0, height = 0;
};

class XrApp {
public:
    bool init(android_app* app);
    void shutdown();

    void pollEvents(bool& exitRequested);
    bool running() const { return running_; }
    bool focused() const { return state_ == XR_SESSION_STATE_FOCUSED; }

    // Runs one frame: prepare(ctx) after view location (may change submitPose), then draw(eye, ctx) per eye.
    void frame(const std::function<void(FrameContext&)>& prepare, const std::function<void(int, const FrameContext&)>& draw);

    // Input, valid after the last frame().
    const std::array<ControllerInput, 2>& controllers() const { return ctrl_; }
    const std::array<HandJoints, 2>& hands() const { return hands_; }
    bool locateHead(XrTime t, XrPosef& out) const;
    bool locateController(int hand, XrTime t, XrPosef& out) const;
    void vibrate(int hand, float amplitude, float frequency, int64_t durationNs);

    // Display.
    const std::vector<float>& refreshRates() const { return rates_; }
    float refreshRate() const;
    void setRefreshRate(float hz);
    uint32_t eyeWidth() const { return static_cast<uint32_t>(eyes_[0].width); }
    uint32_t eyeHeight() const { return static_cast<uint32_t>(eyes_[0].height); }
    bool eyeFovAndIpd(std::array<XrFovf, 2>& fov, float& ipd) const;
    EGLDisplay eglDisplay() const { return display_; }
    XrTime now() const;

private:
    bool initEgl();
    bool createSwapchains();
    void initInput();
    void initHands();
    void syncInput(XrTime t);

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace stage_ = XR_NULL_HANDLE;
    XrSpace view_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool hasRefreshExt_ = false, hasHandExt_ = false, hasTimespec_ = false;
    std::vector<float> rates_;

    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0, height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers;
    };
    std::array<Eye, 2> eyes_;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrPath handPaths_[2] = {};
    XrAction aGrip_ = XR_NULL_HANDLE, aAim_ = XR_NULL_HANDLE, aTrigger_ = XR_NULL_HANDLE, aSqueeze_ = XR_NULL_HANDLE,
             aStick_ = XR_NULL_HANDLE, aStickClick_ = XR_NULL_HANDLE, aStickTouch_ = XR_NULL_HANDLE,
             aTriggerTouch_ = XR_NULL_HANDLE, aBtn1_ = XR_NULL_HANDLE, aBtn1Touch_ = XR_NULL_HANDLE, aBtn2_ = XR_NULL_HANDLE,
             aBtn2Touch_ = XR_NULL_HANDLE, aMenu_ = XR_NULL_HANDLE, aThumbrest_ = XR_NULL_HANDLE, aHaptic_ = XR_NULL_HANDLE;
    XrSpace gripSpace_[2] = {}, aimSpace_[2] = {};
    std::array<ControllerInput, 2> ctrl_{};

    XrHandTrackerEXT handTracker_[2] = {};
    PFN_xrLocateHandJointsEXT locateJoints_ = nullptr;
    std::array<HandJoints, 2> hands_{};
};
