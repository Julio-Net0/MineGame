#ifndef RENDER_GL_INTERNAL_H
#define RENDER_GL_INTERNAL_H

#include "render/gl_api.h"

// Helpers the GL backend shares with its private modules (gl_lod.c). Not part
// of the backend interface.

// Compiles and links a program from two shader files, with the backend's
// shared defines inserted after each #version line. A program the game cannot
// draw without, so a failure ends the process with the driver's log.
GLuint GlLinkShaderProgram(const char *VertexPath, const char *FragmentPath);

#endif
