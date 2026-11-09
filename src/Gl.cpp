#include "Gl.hpp"
#include <android/log.h>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BackgroundVideo", __VA_ARGS__)

namespace bgv::gl {
namespace {

constexpr const char* kVertexShader = R"(
attribute vec2 aPos;
attribute vec2 aUv;
uniform mat4 uTexMatrix;
varying vec2 vUv;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUv = (uTexMatrix * vec4(aUv, 0.0, 1.0)).xy;
}
)";

constexpr const char* kFragmentShader = R"(
#extension GL_OES_EGL_image_external : require
precision mediump float;
varying vec2 vUv;
uniform samplerExternalOES uSampler;
uniform float uBrightness;
void main() {
    gl_FragColor = vec4(texture2D(uSampler, vUv).rgb * uBrightness, 1.0);
}
)";

GLuint compile(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]{};
        glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

}

void save(SavedState& s) {
    glGetIntegerv(GL_CURRENT_PROGRAM, &s.program);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s.vao);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFbo);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_EXTERNAL_OES, &s.externalTexture);
    glGetIntegerv(GL_SAMPLER_BINDING, &s.sampler);
    glGetIntegerv(GL_VIEWPORT, s.viewport);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcAlpha);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstAlpha);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEqRgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEqAlpha);
    s.blend = glIsEnabled(GL_BLEND);
    s.depth = glIsEnabled(GL_DEPTH_TEST);
    s.cull = glIsEnabled(GL_CULL_FACE);
    s.scissor = glIsEnabled(GL_SCISSOR_TEST);
    s.stencil = glIsEnabled(GL_STENCIL_TEST);
    glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
}

void restore(const SavedState& s) {
    auto toggle = [](GLenum cap, GLboolean on) { on ? glEnable(cap) : glDisable(cap); };
    toggle(GL_BLEND, s.blend);
    toggle(GL_DEPTH_TEST, s.depth);
    toggle(GL_CULL_FACE, s.cull);
    toggle(GL_SCISSOR_TEST, s.scissor);
    toggle(GL_STENCIL_TEST, s.stencil);
    glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
    glBlendFuncSeparate(s.blendSrcRgb, s.blendDstRgb, s.blendSrcAlpha, s.blendDstAlpha);
    glBlendEquationSeparate(s.blendEqRgb, s.blendEqAlpha);
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glBindSampler(0, s.sampler);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, s.externalTexture);
    glActiveTexture(s.activeTexture);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, s.drawFbo);
    glBindVertexArray(s.vao);
    glBindBuffer(GL_ARRAY_BUFFER, s.arrayBuffer);
    glUseProgram(s.program);
}

bool Renderer::create() {
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glBindAttribLocation(program, 0, "aPos");
    glBindAttribLocation(program, 1, "aUv");
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]{};
        glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
        LOGE("program link failed: %s", log);
        destroy();
        return false;
    }
    uTexMatrix = glGetUniformLocation(program, "uTexMatrix");
    uBrightness = glGetUniformLocation(program, "uBrightness");
    uSampler = glGetUniformLocation(program, "uSampler");

    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, texture);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 16, nullptr, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 4, reinterpret_cast<void*>(sizeof(float) * 2));
    return glGetError() == GL_NO_ERROR;
}

void Renderer::destroy() {
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (texture) glDeleteTextures(1, &texture);
    if (program) glDeleteProgram(program);
    vbo = vao = texture = program = 0;
}

void Renderer::draw(float sx, float sy, float brightness, const float* texMatrix) {
    const float v[16] = {
        -sx, -sy, 0.f, 0.f,
         sx, -sy, 1.f, 0.f,
        -sx,  sy, 0.f, 1.f,
         sx,  sy, 1.f, 1.f,
    };
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(v), v, GL_STREAM_DRAW);
    glUniformMatrix4fv(uTexMatrix, 1, GL_FALSE, texMatrix);
    glUniform1f(uBrightness, brightness);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

}
