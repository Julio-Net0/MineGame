#ifndef RENDER_GL_API_H
#define RENDER_GL_API_H

// The one place render code gets its OpenGL declarations from. Only the GL
// backend and its private helpers include it.
//
// While the window and context still come from Raylib, its bundled GLAD loader
// (GL 3.3 core, already loaded by Raylib at window creation) supplies the entry
// points. Pulling in a second GLAD instance would link two definitions of every
// glad_gl* pointer into one binary, so the switch to the project's own loader
// happens here, in one line, when the platform moves to GLFW.
#include "external/glad.h"

#endif
