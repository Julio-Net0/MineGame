#include "render/gl_caps.h"

#include "core/log.h"
#include "platform/platform.h"

#include <stdint.h>
#include <string.h>

enum {
  VERSION_ENCODE_MAJOR = 10,
  LOD_REQUIRED_VERSION = 43,
  DRAW_INDIRECT_CORE_VERSION = 40,
  BASE_INSTANCE_CORE_VERSION = 42,
  MULTI_DRAW_INDIRECT_CORE_VERSION = 43,
  CLIP_CONTROL_CORE_VERSION = 45,
  INDIRECT_COUNT_CORE_VERSION = 46,
  ANISOTROPY_CORE_VERSION = 46,
  DRAW_PARAMETERS_CORE_VERSION = 46,
  // GL 4.3 guarantees eight; the LOD renderer's layout stays within them.
  LOD_MIN_STORAGE_BINDINGS = 8,
  INFO_LOG_SIZE = 512,
  MAX_DRAINED_ERRORS = 16
};

// GL_MAX_TEXTURE_MAX_ANISOTROPY: the same value for the EXT and ARB extensions
// and GL 4.6 core. The vendored loader stops at 4.3, so it is not declared.
#define CAPS_MAX_TEXTURE_MAX_ANISOTROPY 0x84FFU

#define PROBE_SEED 0x12345678U
#define PROBE_XOR 0x5A5A5A5AU

static const char PROBE_COMPUTE_SOURCE[] =
    "#version 430 core\n"
    "layout(local_size_x = 1) in;\n"
    "layout(std430, binding = 0) buffer ProbeBuffer { uint Values[2]; };\n"
    "void main() { Values[0] = Values[1] ^ 0x5A5A5A5Au; }\n";

typedef struct {
  RenderCapabilities Caps;
  GlOptionalProcs Procs;
} CapsState;

static CapsState *GetCapsState(void) {
  static CapsState State;
  return &State;
}

const RenderCapabilities *RenderGetCapabilities(void) {
  return &GetCapsState()->Caps;
}

const GlOptionalProcs *GlCapsProcs(void) { return &GetCapsState()->Procs; }

static int EncodeVersion(int Major, int Minor) {
  return (Major * VERSION_ENCODE_MAJOR) + Minor;
}

static bool HasExtension(const char *Name) {
  GLint Count = 0;
  glGetIntegerv(GL_NUM_EXTENSIONS, &Count);
  for (GLint Idx = 0; Idx < Count; Idx++) {
    const GLubyte *Ext = glGetStringi(GL_EXTENSIONS, (GLuint)Idx);
    if (Ext != NULL && strcmp((const char *)Ext, Name) == 0) {
      return true;
    }
  }
  return false;
}

typedef struct {
  int Effective;
  bool Capped;
} VersionGate;

// Core at the effective version, or (without a cap) offered as an extension.
// A cap ignores extensions, so the run matches a driver that stops there.
static bool FeatureAvailable(VersionGate Gate, int CoreVersion,
                             const char *Extension) {
  if (Gate.Effective >= CoreVersion) {
    return true;
  }
  return !Gate.Capped && Extension != NULL && HasExtension(Extension);
}

// Bounded: a lost context can report an error on every call.
static void DrainGlErrors(void) {
  for (int Idx = 0; Idx < MAX_DRAINED_ERRORS; Idx++) {
    if (glGetError() == GL_NO_ERROR) {
      return;
    }
  }
}

static GLuint CompileProbeProgram(void) {
  GLuint Shader = glCreateShader(GL_COMPUTE_SHADER);
  if (Shader == 0) {
    return 0;
  }
  const GLchar *Source = PROBE_COMPUTE_SOURCE;
  glShaderSource(Shader, 1, &Source, NULL);
  glCompileShader(Shader);
  GLint Status = GL_FALSE;
  glGetShaderiv(Shader, GL_COMPILE_STATUS, &Status);
  if (Status != GL_TRUE) {
    char Log[INFO_LOG_SIZE] = {0};
    glGetShaderInfoLog(Shader, (GLsizei)sizeof(Log), NULL, Log);
    LogInfo("RENDER: compute probe did not compile: %s", Log);
    glDeleteShader(Shader);
    return 0;
  }

  GLuint Program = glCreateProgram();
  glAttachShader(Program, Shader);
  glLinkProgram(Program);
  glDetachShader(Program, Shader);
  glDeleteShader(Shader);
  glGetProgramiv(Program, GL_LINK_STATUS, &Status);
  if (Status != GL_TRUE) {
    char Log[INFO_LOG_SIZE] = {0};
    glGetProgramInfoLog(Program, (GLsizei)sizeof(Log), NULL, Log);
    LogInfo("RENDER: compute probe did not link: %s", Log);
    glDeleteProgram(Program);
    return 0;
  }
  return Program;
}

// Dispatches one invocation that writes a value derived from a seed into a
// storage buffer, and reads it back. Proves compile, storage-buffer binding,
// dispatch and the barrier work, not merely that the entry points exist.
static bool ProbeComputeStorage(void) {
  if (glDispatchCompute == NULL || glMemoryBarrier == NULL ||
      glBindBufferBase == NULL || glGetBufferSubData == NULL) {
    return false;
  }
  GLint Bindings = 0;
  glGetIntegerv(GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS, &Bindings);
  if (Bindings < LOD_MIN_STORAGE_BINDINGS) {
    LogInfo("RENDER: %d storage buffer bindings, %d needed", (int)Bindings,
            LOD_MIN_STORAGE_BINDINGS);
    return false;
  }

  DrainGlErrors();
  GLuint Program = CompileProbeProgram();
  if (Program == 0) {
    return false;
  }

  uint32_t Initial[2] = {0U, PROBE_SEED};
  uint32_t Result[2] = {0U, 0U};
  GLuint Buffer = 0;
  glGenBuffers(1, &Buffer);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, Buffer);
  glBufferData(GL_SHADER_STORAGE_BUFFER, (GLsizeiptr)sizeof(Initial), Initial,
               GL_DYNAMIC_COPY);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, Buffer);
  glUseProgram(Program);
  glDispatchCompute(1, 1, 1);
  glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
  glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, (GLsizeiptr)sizeof(Result),
                     Result);

  glUseProgram(0);
  glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, 0);
  glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
  glDeleteBuffers(1, &Buffer);
  glDeleteProgram(Program);

  GLenum Error = glGetError();
  DrainGlErrors();
  bool Passed = Error == GL_NO_ERROR && Result[0] == (PROBE_SEED ^ PROBE_XOR);
  if (!Passed) {
    LogInfo("RENDER: compute probe read 0x%08X (GL error 0x%04X)",
            (unsigned)Result[0], (unsigned)Error);
  }
  return Passed;
}

static void LoadOptionalProcs(CapsState *State, VersionGate Gate) {
  RenderCapabilities *Caps = &State->Caps;
  GlOptionalProcs *Procs = &State->Procs;
  *Procs = (GlOptionalProcs){0};

  if (FeatureAvailable(Gate, INDIRECT_COUNT_CORE_VERSION,
                       "GL_ARB_indirect_parameters")) {
    const char *Name = (Gate.Effective >= INDIRECT_COUNT_CORE_VERSION)
                           ? "glMultiDrawElementsIndirectCount"
                           : "glMultiDrawElementsIndirectCountARB";
    Procs->MultiDrawElementsIndirectCount =
        (GlMultiDrawElementsIndirectCountProc)PlatformGetProcAddress(Name);
  }
  Caps->IndirectCount = Procs->MultiDrawElementsIndirectCount != NULL;

  if (FeatureAvailable(Gate, CLIP_CONTROL_CORE_VERSION, "GL_ARB_clip_control")) {
    Procs->ClipControl =
        (GlClipControlProc)PlatformGetProcAddress("glClipControl");
  }
  Caps->ClipControl = Procs->ClipControl != NULL;

  Caps->ShaderDrawParameters = FeatureAvailable(
      Gate, DRAW_PARAMETERS_CORE_VERSION, "GL_ARB_shader_draw_parameters");

  Caps->MaxAnisotropy = 1.0F;
  if (FeatureAvailable(Gate, ANISOTROPY_CORE_VERSION,
                       "GL_ARB_texture_filter_anisotropic") ||
      FeatureAvailable(Gate, ANISOTROPY_CORE_VERSION,
                       "GL_EXT_texture_filter_anisotropic")) {
    GLfloat Max = 1.0F;
    glGetFloatv(CAPS_MAX_TEXTURE_MAX_ANISOTROPY, &Max);
    if (glGetError() == GL_NO_ERROR && Max > 1.0F) {
      Caps->MaxAnisotropy = Max;
    }
  }
}

static const char *FindLodBlocker(const RenderCapabilities *Caps,
                                  int Effective) {
  if (Effective < LOD_REQUIRED_VERSION) {
    return (Caps->VersionCapped) ? "OpenGL version capped below 4.3"
                                 : "OpenGL 4.3 not available";
  }
  if (!Caps->MultiDrawIndirect) {
    return "multi-draw indirect not available";
  }
  if (!Caps->BaseInstance) {
    return "base instance draws not available";
  }
  if (!Caps->ComputeShaders) {
    return "compute shader / storage buffer probe failed";
  }
  return "";
}

void GlCapsDetect(int VersionCap) {
  CapsState *State = GetCapsState();
  RenderCapabilities *Caps = &State->Caps;
  *Caps = (RenderCapabilities){0};

  const PlatformGraphicsContext *Context = PlatformGetGraphicsContext();
  Caps->RequestedMajor = Context->RequestedMajor;
  Caps->RequestedMinor = Context->RequestedMinor;
  Caps->ContextFellBack = Context->FellBack;

  GLint Major = 0;
  GLint Minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &Major);
  glGetIntegerv(GL_MINOR_VERSION, &Minor);
  Caps->ContextMajor = (int)Major;
  Caps->ContextMinor = (int)Minor;

  // Any cap ignores extensions, even when the driver already provides no more
  // than the cap: some drivers grant exactly the requested version yet still
  // list newer features as extensions, which a real driver of that age lacks.
  int Provided = EncodeVersion((int)Major, (int)Minor);
  VersionGate Gate = {.Effective = Provided, .Capped = VersionCap > 0};
  if (Gate.Capped && VersionCap < Provided) {
    Gate.Effective = VersionCap;
  }
  Caps->VersionCapped = Gate.Capped;
  Caps->EffectiveMajor = Gate.Effective / VERSION_ENCODE_MAJOR;
  Caps->EffectiveMinor = Gate.Effective % VERSION_ENCODE_MAJOR;

  // Multi-draw indirect reads the draw indirect buffer, which is core in 4.0
  // or comes with ARB_draw_indirect.
  bool DrawIndirect = FeatureAvailable(Gate, DRAW_INDIRECT_CORE_VERSION,
                                       "GL_ARB_draw_indirect");
  Caps->MultiDrawIndirect =
      DrawIndirect &&
      FeatureAvailable(Gate, MULTI_DRAW_INDIRECT_CORE_VERSION,
                       "GL_ARB_multi_draw_indirect") &&
      glMultiDrawElementsIndirect != NULL;
  Caps->BaseInstance =
      FeatureAvailable(Gate, BASE_INSTANCE_CORE_VERSION,
                       "GL_ARB_base_instance") &&
      glDrawElementsInstancedBaseVertexBaseInstance != NULL;
  Caps->ComputeShaders =
      Gate.Effective >= LOD_REQUIRED_VERSION && ProbeComputeStorage();

  LoadOptionalProcs(State, Gate);

  Caps->LodUnavailableReason = FindLodBlocker(Caps, Gate.Effective);
  Caps->LodSupported = Caps->LodUnavailableReason[0] == '\0';

  LogInfo("RENDER: OpenGL %d.%d requested, %d.%d provided, %d.%d effective%s; "
          "mdi %d, base instance %d, compute %d, indirect count %d, clip "
          "control %d, draw parameters %d, anisotropy %.0f",
          Caps->RequestedMajor, Caps->RequestedMinor, Caps->ContextMajor,
          Caps->ContextMinor, Caps->EffectiveMajor, Caps->EffectiveMinor,
          Caps->VersionCapped ? " (--gl-max)" : "", (int)Caps->MultiDrawIndirect,
          (int)Caps->BaseInstance, (int)Caps->ComputeShaders,
          (int)Caps->IndirectCount, (int)Caps->ClipControl,
          (int)Caps->ShaderDrawParameters, (double)Caps->MaxAnisotropy);
  if (Caps->LodSupported) {
    LogInfo("RENDER: LOD supported");
  } else {
    LogInfo("RENDER: LOD unavailable: %s", Caps->LodUnavailableReason);
  }
}
