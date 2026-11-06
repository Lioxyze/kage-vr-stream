// GLES 3 renderer: streamed video (external OES texture), and a light lobby
// scene (cubes, quads, lines, text from the system Roboto font).
#pragma once

#include <GLES3/gl3.h>

#include <string_view>
#include <vector>

#include "xr_math.hpp"

struct Color {
    float r, g, b, a;
};

namespace palette {
inline constexpr Color kBg{0.02f, 0.01f, 0.05f, 1};
inline constexpr Color kPanel{0.07f, 0.06f, 0.10f, 0.92f};
inline constexpr Color kViolet{0.55f, 0.30f, 1.0f, 1};
inline constexpr Color kCyan{0.0f, 0.85f, 0.95f, 1};
inline constexpr Color kText{0.92f, 0.92f, 0.96f, 1};
inline constexpr Color kDim{0.55f, 0.55f, 0.62f, 1};
inline constexpr Color kWarn{1.0f, 0.62f, 0.15f, 1};
}  // namespace palette

class Renderer {
public:
    bool init();
    void setViewProj(const xm::Mat4& vp) { vp_ = vp; }

    void cube(const xm::Mat4& model, Color c);
    void quad(const xm::Mat4& model, Color c);  // unit quad in XY plane, centred
    void lines(const std::vector<XrVector3f>& pts, Color c);  // pairs
    // Text in the XY plane of `model` (meters). x,y = baseline start; size = cap height in meters.
    void text(const xm::Mat4& model, float x, float y, float size, std::string_view utf8, Color c);
    float textWidth(std::string_view utf8, float size) const;

    // Full-viewport draw of half of a side-by-side external OES video frame.
    void video(GLuint oesTexture, int eye, float uScale);

private:
    GLuint colorProg_ = 0, textProg_ = 0, videoProg_ = 0;
    GLint cMvp_ = -1, cColor_ = -1, tMvp_ = -1, tColor_ = -1, vRect_ = -1;
    GLuint cubeVbo_ = 0, quadVbo_ = 0, dynVbo_ = 0, vao_ = 0;
    GLuint atlas_ = 0;
    bool hasFont_ = false;
    xm::Mat4 vp_{};
    std::vector<float> scratch_;
};
