#ifndef RENDER_GL_CAPS_H
#define RENDER_GL_CAPS_H

#include "render/backend.h"
#include "render/gl_api.h"

// Capability detection for the GL backend, private to it.
//
// Entry points of optional features outside the vendored GLAD loader's set are
// resolved here by name, and only after the extension or core version that
// provides them has been confirmed; a pointer is NULL whenever its capability
// flag is false.

typedef void(GLAD_API_PTR *GlMultiDrawElementsIndirectCountProc)(
    GLenum Mode, GLenum Type, const void *Indirect, GLintptr DrawCount,
    GLsizei MaxDrawCount, GLsizei Stride);
typedef void(GLAD_API_PTR *GlClipControlProc)(GLenum Origin, GLenum Depth);

typedef struct {
  GlMultiDrawElementsIndirectCountProc MultiDrawElementsIndirectCount;
  GlClipControlProc ClipControl;
} GlOptionalProcs;

// Detects everything once, with the context current. VersionCap is
// major * 10 + minor, or 0 for no cap. LodAllowed false (--lod off) reports
// LOD as unsupported whatever the driver offers. Leaves no GL binding changed.
void GlCapsDetect(int VersionCap, bool LodAllowed);

const GlOptionalProcs *GlCapsProcs(void);

#endif
