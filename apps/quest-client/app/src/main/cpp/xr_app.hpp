// KAGE VR Stream Quest client - minimal OpenXR (OpenGL ES) application.
// Phase 1: creates the session, a stereo swapchain per eye, and submits a
// projection layer every frame (solid colour reflecting the network state).
// It also logs the real capabilities reported by the runtime (recommended
// per-eye resolution, supported refresh rates) to validate them on device.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

struct android_app;

class XrApp {
public:
    bool init(android_app* app);
    void shutdown();

    // Processes runtime events. Sets exitRequested when the app must quit.
    void pollEvents(bool& exitRequested);
    bool running() const { return running_; }

    // Renders and submits one frame. Must only be called while running().
    void renderFrame(const std::array<float, 4>& clearColor);

    // Head pose from the last rendered frame (LOCAL space). Valid when headValid().
    bool headValid() const { return headValid_; }
    XrPosef headPose() const { return headPose_; }

private:
    bool initEgl();
    bool createSwapchains();
    void logRefreshRates();

    EGLDisplay display_ = EGL_NO_DISPLAY;
    EGLConfig config_ = nullptr;
    EGLContext context_ = EGL_NO_CONTEXT;
    EGLSurface surface_ = EGL_NO_SURFACE;

    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace localSpace_ = XR_NULL_HANDLE;
    XrSpace viewSpace_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool hasRefreshRateExt_ = false;

    struct Eye {
        XrSwapchain swapchain = XR_NULL_HANDLE;
        int32_t width = 0, height = 0;
        std::vector<XrSwapchainImageOpenGLESKHR> images;
        std::vector<GLuint> framebuffers;
    };
    std::array<Eye, 2> eyes_;

    bool headValid_ = false;
    XrPosef headPose_{{0, 0, 0, 1}, {0, 0, 0}};
};
