#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

namespace bgv::gl {

// Everything we change while drawing, so the game never notices.
struct SavedState {
    GLint program = 0, arrayBuffer = 0, vao = 0, drawFbo = 0, activeTexture = 0, externalTexture = 0, sampler = 0;
    GLint viewport[4]{};
    GLint blendSrcRgb = 0, blendDstRgb = 0, blendSrcAlpha = 0, blendDstAlpha = 0;
    GLint blendEqRgb = 0, blendEqAlpha = 0;
    GLboolean blend = GL_FALSE, depth = GL_FALSE, cull = GL_FALSE, scissor = GL_FALSE, stencil = GL_FALSE;
    GLboolean colorMask[4]{};
};

void save(SavedState& state);
void restore(const SavedState& state);

// External-texture quad renderer (fed by a SurfaceTexture).
struct Renderer {
    GLuint program = 0, texture = 0, vbo = 0, vao = 0;
    GLint uTexMatrix = -1, uBrightness = -1, uSampler = -1;

    bool create();
    void destroy();
    bool ready() const { return program != 0; }
    // Draws a centered quad covering [-sx, sx] x [-sy, sy] in clip space.
    void draw(float sx, float sy, float brightness, const float* texMatrix);
};

}
