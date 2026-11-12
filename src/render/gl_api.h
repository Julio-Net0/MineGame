#ifndef RENDER_GL_API_H
#define RENDER_GL_API_H

// The one place render code gets its OpenGL declarations from. Only the GL
// backend and its private helpers include it. The entry points are the
// vendored GLAD loader's (src/third_party/glad), which the platform loads once
// the context exists.
#include <glad/gl.h>

#endif
