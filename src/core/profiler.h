#ifndef CORE_PROFILER_H
#define CORE_PROFILER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

// Renderer-free scope timing. Measures the wall-clock cost of named subsystems
// and aggregates it over a rolling window, so a slow frame can be attributed to
// the subsystem that caused it.
//
// Wall-clock, not CPU time: a scope descheduled by the OS reports the time it
// was not running. That is deliberate — it is what the player experiences, and
// it is the right measure for "why did this frame take 200 ms". Readings are
// only comparable against other readings taken under similar load.
//
// Time is read through the platform clock abstraction, so simulation-layer code
// can be instrumented without linking the renderer. This header pulls in no
// renderer, windowing, or OS header.
//
// Instrument coarsely. A scope costs two clock reads; against a whole chunk
// mesh build that is noise, against a per-face or per-block loop it would be
// the measurement. Scopes belong around whole subsystem operations only.

// Scopes must not nest. The frame total minus the sum of the main-thread scopes
// is read as "time in no scope at all", which is how an unaccounted stall gets
// found; a nested scope would be counted twice and make that remainder lie.
typedef enum {
  PROFILE_MESH_BUILD = 0,
  PROFILE_MESH_UPLOAD,
  PROFILE_WORLD_DRAW,
  PROFILE_WORLD_UPDATE,
  PROFILE_FRAME_SETUP,
  PROFILE_HUD,
  PROFILE_PRESENT,
  PROFILE_PLAYER,
  PROFILE_WORKER_GENERATION,
  PROFILE_SCOPE_COUNT
} ProfileScope;

// Aggregated over the profiler's rolling window. Average alone would hide the
// stall this instrument exists to find, so the peak and the 1% low — the mean
// of the worst one percent of frames — are reported alongside it.
typedef struct {
  uint32_t AverageUs;
  uint32_t MaxUs;
  uint32_t OnePercentLowUs;
  uint32_t AverageEntries;
} ProfileStats;

const char *GetProfileScopeName(ProfileScope Scope);

#ifdef MINEGAME_PROFILE

typedef enum {
  PROFILER_DUMP_OK = 0,
  PROFILER_DUMP_NO_SAMPLES,
  PROFILER_DUMP_WRITE_FAILED
} ProfilerDumpResult;

enum {
  PROFILER_DUMP_PATH_CAPACITY = 64
};

// Write the rolling window as CSV, one row per recorded frame, oldest first.
// The aggregates on the overlay say that a stall happened; only the per-frame
// series says what shape it has — one spike or a sustained plateau — and how
// much work each slow frame carried, which are different diagnoses.
//
// OutPath receives the file actually written and must hold at least
// PROFILER_DUMP_PATH_CAPACITY chars. Each call writes a new file rather than
// overwriting a previous one.
//
// The window holds a fixed number of FRAMES, so the wall time it spans depends
// entirely on the framerate — under a second on a fast machine, minutes on a
// slow one. Use the streaming capture below when the goal is to record a
// specific activity rather than whatever just happened.
ProfilerDumpResult ProfilerDumpCsv(char *OutPath, size_t PathCapacity);

// Streaming capture: append one row per frame to a file until stopped. Unlike
// the window dump this has no length limit, so a capture covers exactly the
// activity the player performed between start and stop rather than the last N
// frames — which is what makes it usable on a machine fast enough that N frames
// is a fraction of a second.
//
// Starting while already capturing is a no-op that reports failure. OutPath
// receives the file being written.
bool ProfilerStartCapture(char *OutPath, size_t PathCapacity);
void ProfilerStopCapture(void);
bool ProfilerIsCapturing(void);

// Wall-clock duration of the whole frame, measured between consecutive frame
// boundaries. Reported alongside the scopes so the share of the frame they do
// NOT account for is visible rather than inferred.
void ProfilerQueryFrame(ProfileStats *Out);

double ProfilerNow(void);
void ProfilerRecord(ProfileScope Scope, double StartTime);
void ProfilerEndFrame(void);
void ProfilerQuery(ProfileScope Scope, ProfileStats *Out);

// Open and close a scope. The identifier names a local holding the start time,
// so nested scopes in one function simply use different identifiers. The local
// is deliberately not const: a const local reads as a constant to the project's
// naming rules, which would demand an UPPER_CASE name and so misrepresent a
// per-call timestamp as a compile-time constant.
#define PROFILE_BEGIN(Var) double Var = ProfilerNow()
#define PROFILE_END(Var, Scope) ProfilerRecord((Scope), (Var))
#define PROFILE_FRAME_END() ProfilerEndFrame()

#else

// Compiled out entirely: no clock read, no storage, no branch. PROFILE_BEGIN
// declares nothing, which is safe because PROFILE_END expands to nothing too
// and is the only thing that would have referenced it.
#define PROFILE_BEGIN(Var) ((void)0)
#define PROFILE_END(Var, Scope) ((void)0)
#define PROFILE_FRAME_END() ((void)0)

#endif

#endif
