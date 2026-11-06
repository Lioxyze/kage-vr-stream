#include "renderer.hpp"

#include <GLES2/gl2ext.h>

#include <cstdio>
#include <vector>

#include "kage/util/log.hpp"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace klog = kage::log;

namespace {

constexpr int kAtlas = 1024;
constexpr float kBakePx = 56.f;
stbtt_bakedchar g_chars[224];  // code points 32..255 (ASCII + Latin-1 for French)

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        klog::error("shader: {}", log);
    }
    return s;
}

GLuint program(const char* vs, const char* fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "aPos");
    glBindAttribLocation(p, 1, "aUv");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) klog::error("program link failed");
    return p;
}

// Swapchains are sRGB: the hardware encodes linear->sRGB on write, so colours
// given in sRGB (and the gamma-encoded video) are linearised in the shaders.
const char* kColorVs = R"(#version 300 es
layout(location=0) in vec3 aPos; uniform mat4 uMvp;
void main(){ gl_Position = uMvp * vec4(aPos,1.0); })";
const char* kColorFs = R"(#version 300 es
precision mediump float; uniform vec4 uColor; out vec4 o;
void main(){ o = vec4(pow(uColor.rgb, vec3(2.2)), uColor.a); })";
const char* kTextVs = R"(#version 300 es
layout(location=0) in vec3 aPos; layout(location=1) in vec2 aUv; uniform mat4 uMvp; out vec2 vUv;
void main(){ vUv = aUv; gl_Position = uMvp * vec4(aPos,1.0); })";
const char* kTextFs = R"(#version 300 es
precision mediump float; uniform sampler2D uTex; uniform vec4 uColor; in vec2 vUv; out vec4 o;
void main(){ float a = texture(uTex, vUv).r; o = vec4(pow(uColor.rgb, vec3(2.2)), uColor.a * a); })";
const char* kVideoVs = R"(#version 300 es
uniform vec4 uRect; out vec2 vUv;
void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
  vUv = vec2(mix(uRect.x, uRect.z, p.x), mix(uRect.w, uRect.y, p.y));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); })";
const char* kVideoFs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float; uniform samplerExternalOES uTex; in vec2 vUv; out vec4 o;
vec3 toLinear(vec3 c){ return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
void main(){ o = vec4(toLinear(texture(uTex, vUv).rgb), 1.0); })";

int latin1(std::string_view s, size_t& i) {
    const auto c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && i < s.size()) {
        const int cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
        return cp <= 255 ? cp : '?';
    }
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;  // skip other sequences
    return '?';
}

}  // namespace

bool Renderer::init() {
    colorProg_ = program(kColorVs, kColorFs);
    textProg_ = program(kTextVs, kTextFs);
    videoProg_ = program(kVideoVs, kVideoFs);
    cMvp_ = glGetUniformLocation(colorProg_, "uMvp");
    cColor_ = glGetUniformLocation(colorProg_, "uColor");
    tMvp_ = glGetUniformLocation(textProg_, "uMvp");
    tColor_ = glGetUniformLocation(textProg_, "uColor");
    vRect_ = glGetUniformLocation(videoProg_, "uRect");

    static const float cube[] = {
        -.5f, -.5f, .5f, .5f, -.5f, .5f, .5f, .5f, .5f, -.5f, -.5f, .5f, .5f, .5f, .5f, -.5f, .5f, .5f,
        .5f, -.5f, -.5f, -.5f, -.5f, -.5f, -.5f, .5f, -.5f, .5f, -.5f, -.5f, -.5f, .5f, -.5f, .5f, .5f, -.5f,
        -.5f, -.5f, -.5f, -.5f, -.5f, .5f, -.5f, .5f, .5f, -.5f, -.5f, -.5f, -.5f, .5f, .5f, -.5f, .5f, -.5f,
        .5f, -.5f, .5f, .5f, -.5f, -.5f, .5f, .5f, -.5f, .5f, -.5f, .5f, .5f, .5f, -.5f, .5f, .5f, .5f,
        -.5f, .5f, .5f, .5f, .5f, .5f, .5f, .5f, -.5f, -.5f, .5f, .5f, .5f, .5f, -.5f, -.5f, .5f, -.5f,
        -.5f, -.5f, -.5f, .5f, -.5f, -.5f, .5f, -.5f, .5f, -.5f, -.5f, -.5f, .5f, -.5f, .5f, -.5f, -.5f, .5f};
    static const float quad[] = {-.5f, -.5f, 0, .5f, -.5f, 0, .5f, .5f, 0, -.5f, -.5f, 0, .5f, .5f, 0, -.5f, .5f, 0};
    glGenVertexArrays(1, &vao_);
    glBindVertexArray(vao_);
    glGenBuffers(1, &cubeVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
    glGenBuffers(1, &quadVbo_);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    glGenBuffers(1, &dynVbo_);

    // Font atlas from the system font (no asset shipped in the APK).
    for (const char* path : {"/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf"}) {
        FILE* f = std::fopen(path, "rb");
        if (!f) continue;
        std::fseek(f, 0, SEEK_END);
        std::vector<unsigned char> ttf(static_cast<size_t>(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        std::fread(ttf.data(), 1, ttf.size(), f);
        std::fclose(f);
        std::vector<unsigned char> bmp(kAtlas * kAtlas);
        if (stbtt_BakeFontBitmap(ttf.data(), 0, kBakePx, bmp.data(), kAtlas, kAtlas, 32, 224, g_chars) > 0) {
            glGenTextures(1, &atlas_);
            glBindTexture(GL_TEXTURE_2D, atlas_);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, kAtlas, kAtlas, 0, GL_RED, GL_UNSIGNED_BYTE, bmp.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            hasFont_ = true;
            klog::info("font atlas built from {}", path);
            break;
        }
    }
    if (!hasFont_) klog::warn("no system font found: text disabled");
    return colorProg_ && textProg_ && videoProg_;
}

void Renderer::cube(const xm::Mat4& model, Color c) {
    glUseProgram(colorProg_);
    const auto mvp = xm::mul(vp_, model);
    glUniformMatrix4fv(cMvp_, 1, GL_FALSE, mvp.m);
    glUniform4f(cColor_, c.r, c.g, c.b, c.a);
    glBindBuffer(GL_ARRAY_BUFFER, cubeVbo_);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 36);
}

void Renderer::quad(const xm::Mat4& model, Color c) {
    glUseProgram(colorProg_);
    const auto mvp = xm::mul(vp_, model);
    glUniformMatrix4fv(cMvp_, 1, GL_FALSE, mvp.m);
    glUniform4f(cColor_, c.r, c.g, c.b, c.a);
    glBindBuffer(GL_ARRAY_BUFFER, quadVbo_);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void Renderer::lines(const std::vector<XrVector3f>& pts, Color c) {
    if (pts.size() < 2) return;
    glUseProgram(colorProg_);
    glUniformMatrix4fv(cMvp_, 1, GL_FALSE, vp_.m);
    glUniform4f(cColor_, c.r, c.g, c.b, c.a);
    glBindBuffer(GL_ARRAY_BUFFER, dynVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(pts.size() * sizeof(XrVector3f)), pts.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(pts.size()));
}

float Renderer::textWidth(std::string_view s, float size) const {
    if (!hasFont_) return 0;
    float x = 0, y = 0;
    for (size_t i = 0; i < s.size();) {
        const int cp = latin1(s, i);
        if (cp < 32 || cp > 255) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_chars, kAtlas, kAtlas, cp - 32, &x, &y, &q, 1);
    }
    return x * size / (kBakePx * 0.7f);
}

void Renderer::text(const xm::Mat4& model, float x0, float y0, float size, std::string_view s, Color c) {
    if (!hasFont_ || s.empty()) return;
    const float k = size / (kBakePx * 0.7f);  // cap height ~0.7 em
    scratch_.clear();
    float x = 0, y = 0;
    for (size_t i = 0; i < s.size();) {
        const int cp = latin1(s, i);
        if (cp < 32 || cp > 255) continue;
        stbtt_aligned_quad q;
        stbtt_GetBakedQuad(g_chars, kAtlas, kAtlas, cp - 32, &x, &y, &q, 1);
        const float X0 = x0 + q.x0 * k, X1 = x0 + q.x1 * k, Y0 = y0 - q.y0 * k, Y1 = y0 - q.y1 * k;
        const float v[] = {X0, Y0, 0, q.s0, q.t0, X1, Y0, 0, q.s1, q.t0, X1, Y1, 0, q.s1, q.t1,
                           X0, Y0, 0, q.s0, q.t0, X1, Y1, 0, q.s1, q.t1, X0, Y1, 0, q.s0, q.t1};
        scratch_.insert(scratch_.end(), std::begin(v), std::end(v));
    }
    glUseProgram(textProg_);
    const auto mvp = xm::mul(vp_, model);
    glUniformMatrix4fv(tMvp_, 1, GL_FALSE, mvp.m);
    glUniform4f(tColor_, c.r, c.g, c.b, c.a);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, atlas_);
    glBindBuffer(GL_ARRAY_BUFFER, dynVbo_);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(scratch_.size() * sizeof(float)), scratch_.data(), GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, nullptr);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, reinterpret_cast<void*>(12));
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(scratch_.size() / 5));
}

void Renderer::video(GLuint tex, int eye, float uScale) {
    glUseProgram(videoProg_);
    const float u0 = eye * 0.5f * uScale, u1 = u0 + 0.5f * uScale;
    glUniform4f(vRect_, u0, 0.f, u1, 1.f);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glDisableVertexAttribArray(0);
    glDisableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}
