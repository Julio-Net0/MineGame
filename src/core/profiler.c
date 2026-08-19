#include "core/profiler.h"

#include "platform/platform.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Kept within twelve characters: the overlay lays the panel out in fixed
// columns and a longer name would push the numbers out of alignment.
static const char *const SCOPE_NAMES[PROFILE_SCOPE_COUNT] = {
    "Mesh build", "Mesh upload", "World draw", "World update", "Frame setup",
    "HUD 2D",     "Present",     "Player",     "Worker gen"};

const char *GetProfileScopeName(ProfileScope Scope) {
  if (Scope < 0 || Scope >= PROFILE_SCOPE_COUNT) {
    return "?";
  }
  return SCOPE_NAMES[Scope];
}

#ifdef MINEGAME_PROFILE

enum {
  // 1% of the window has to be more than one frame for a 1% low to be a
  // statistic rather than a single outlier. At 1024 frames it is about ten,
  // and the window spans roughly 17 seconds at 60 FPS — the right order for
  // "did this stall happen recently".
  PROFILER_WINDOW_FRAMES = 1024,
  PROFILER_LOW_PERCENT_DIVISOR = 100,
  PROFILER_TOP_CAPACITY = (PROFILER_WINDOW_FRAMES / PROFILER_LOW_PERCENT_DIVISOR) + 1
};

#define MICROSECONDS_PER_SECOND 1000000.0

typedef struct {
  // Written by any thread through relaxed fetch-add, drained by the main thread
  // at the frame boundary. A worker sample racing the drain lands in the next
  // frame instead of this one, which loses nothing and costs no lock.
  atomic_uint_least64_t AccumUs[PROFILE_SCOPE_COUNT];
  atomic_uint_least32_t AccumEntries[PROFILE_SCOPE_COUNT];

  // Main-thread only: written by ProfilerEndFrame, read by ProfilerQuery.
  uint32_t WindowUs[PROFILE_SCOPE_COUNT][PROFILER_WINDOW_FRAMES];
  uint32_t WindowEntries[PROFILE_SCOPE_COUNT][PROFILER_WINDOW_FRAMES];

  // Whole-frame wall clock, measured between consecutive frame boundaries. Held
  // separately from the scopes because it is not one: it is the total the scopes
  // are a part of, and reporting it makes the unaccounted remainder visible.
  uint32_t WindowFrameUs[PROFILER_WINDOW_FRAMES];
  double LastFrameEndTime;

  int WriteCursor;
  int RecordedFrames;

  // Streaming capture, main-thread only: opened by ProfilerStartCapture,
  // appended to by ProfilerEndFrame, closed by ProfilerStopCapture.
  FILE *CaptureFile;
  int CaptureRow;
} ProfilerState;

static ProfilerState *GetProfilerState(void) {
  static ProfilerState State;
  return &State;
}

double ProfilerNow(void) { return PlatformGetTime(); }

void ProfilerRecord(ProfileScope Scope, double StartTime) {
  if (Scope < 0 || Scope >= PROFILE_SCOPE_COUNT) {
    return;
  }

  double Elapsed = PlatformGetTime() - StartTime;
  if (Elapsed < 0.0) {
    Elapsed = 0.0;
  }

  ProfilerState *State = GetProfilerState();
  uint_least64_t ElapsedUs = (uint_least64_t)(Elapsed * MICROSECONDS_PER_SECOND);

  atomic_fetch_add_explicit(&State->AccumUs[Scope], ElapsedUs,
                            memory_order_relaxed);
  atomic_fetch_add_explicit(&State->AccumEntries[Scope], 1U,
                            memory_order_relaxed);
}

static void WriteCsvHeader(FILE *FileVal);
static void WriteCsvRow(FILE *FileVal, const ProfilerState *State, int Row,
                        int Slot);

void ProfilerEndFrame(void) {
  ProfilerState *State = GetProfilerState();
  int Slot = State->WriteCursor;

  // Between boundaries, so it covers the whole frame including anything outside
  // every scope. The very first frame has no previous boundary to measure from
  // and records zero rather than a meaningless interval since process start.
  double Now = PlatformGetTime();
  double FrameSeconds =
      (State->LastFrameEndTime > 0.0) ? (Now - State->LastFrameEndTime) : 0.0;
  if (FrameSeconds < 0.0) {
    FrameSeconds = 0.0;
  }
  State->LastFrameEndTime = Now;

  uint_least64_t FrameUs =
      (uint_least64_t)(FrameSeconds * MICROSECONDS_PER_SECOND);
  State->WindowFrameUs[Slot] =
      (FrameUs > UINT32_MAX) ? UINT32_MAX : (uint32_t)FrameUs;

  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    uint_least64_t TotalUs = atomic_exchange_explicit(&State->AccumUs[Scope], 0U,
                                                      memory_order_relaxed);
    uint_least32_t Entries = atomic_exchange_explicit(
        &State->AccumEntries[Scope], 0U, memory_order_relaxed);

    // Saturate rather than wrap. Four workers can sum to more than a uint32 of
    // microseconds only after roughly an hour inside one scope in one frame,
    // but a wrapped sample would read as a suspiciously fast frame, which is
    // the one failure mode this instrument must never produce.
    State->WindowUs[Scope][Slot] =
        (TotalUs > UINT32_MAX) ? UINT32_MAX : (uint32_t)TotalUs;
    State->WindowEntries[Scope][Slot] = (uint32_t)Entries;
  }

  // Appended after the slot is complete, so a streamed row and the window hold
  // identical data. The write itself lands outside every scope and is charged to
  // the next frame's duration; buffered, it is well under a microsecond against
  // frames measured in hundreds.
  if (State->CaptureFile != NULL) {
    WriteCsvRow(State->CaptureFile, State, State->CaptureRow, Slot);
    State->CaptureRow++;
  }

  State->WriteCursor = (Slot + 1) % PROFILER_WINDOW_FRAMES;
  if (State->RecordedFrames < PROFILER_WINDOW_FRAMES) {
    State->RecordedFrames++;
  }
}

// Keep the Capacity largest values seen, sorted ascending, so Top[0] is always
// the smallest of the current set and the admission test is a single compare.
// A full sort of the window would do the same job; this avoids qsort's
// per-comparison indirect call for a fraction of the work.
static void PushTopK(uint32_t *Top, int Capacity, int *Count, uint32_t Value) {
  if (*Count < Capacity) {
    int Idx = *Count;
    while (Idx > 0 && Top[Idx - 1] > Value) {
      Top[Idx] = Top[Idx - 1];
      Idx--;
    }
    Top[Idx] = Value;
    (*Count)++;
    return;
  }

  if (Value <= Top[0]) {
    return;
  }

  int Idx = 0;
  while (Idx + 1 < Capacity && Top[Idx + 1] < Value) {
    Top[Idx] = Top[Idx + 1];
    Idx++;
  }
  Top[Idx] = Value;
}

void ProfilerQuery(ProfileScope Scope, ProfileStats *Out) {
  if (Out == NULL) {
    return;
  }

  Out->AverageUs = 0;
  Out->MaxUs = 0;
  Out->OnePercentLowUs = 0;
  Out->AverageEntries = 0;

  if (Scope < 0 || Scope >= PROFILE_SCOPE_COUNT) {
    return;
  }

  const ProfilerState *State = GetProfilerState();
  int Frames = State->RecordedFrames;
  if (Frames <= 0) {
    return;
  }

  // Over the frames actually recorded, not the whole window: before the ring
  // fills, the unwritten slots are zeros that would drag every average down.
  int TopCapacity = Frames / PROFILER_LOW_PERCENT_DIVISOR;
  if (TopCapacity < 1) {
    TopCapacity = 1;
  }

  uint32_t Top[PROFILER_TOP_CAPACITY];
  int TopCount = 0;
  uint64_t SumUs = 0;
  uint64_t SumEntries = 0;
  uint32_t MaxUs = 0;

  for (int Idx = 0; Idx < Frames; Idx++) {
    uint32_t Sample = State->WindowUs[Scope][Idx];
    SumUs += Sample;
    SumEntries += State->WindowEntries[Scope][Idx];
    if (Sample > MaxUs) {
      MaxUs = Sample;
    }
    PushTopK(Top, TopCapacity, &TopCount, Sample);
  }

  uint64_t SumTop = 0;
  for (int Idx = 0; Idx < TopCount; Idx++) {
    SumTop += Top[Idx];
  }

  Out->AverageUs = (uint32_t)(SumUs / (uint64_t)Frames);
  Out->MaxUs = MaxUs;
  Out->OnePercentLowUs =
      (TopCount > 0) ? (uint32_t)(SumTop / (uint64_t)TopCount) : 0U;
  Out->AverageEntries = (uint32_t)(SumEntries / (uint64_t)Frames);
}

// Same shape as ProfilerQuery but over the whole-frame series, which is not a
// scope and so has no entry count.
void ProfilerQueryFrame(ProfileStats *Out) {
  if (Out == NULL) {
    return;
  }

  Out->AverageUs = 0;
  Out->MaxUs = 0;
  Out->OnePercentLowUs = 0;
  Out->AverageEntries = 0;

  const ProfilerState *State = GetProfilerState();
  int Frames = State->RecordedFrames;
  if (Frames <= 0) {
    return;
  }

  int TopCapacity = Frames / PROFILER_LOW_PERCENT_DIVISOR;
  if (TopCapacity < 1) {
    TopCapacity = 1;
  }

  uint32_t Top[PROFILER_TOP_CAPACITY];
  int TopCount = 0;
  uint64_t SumUs = 0;
  uint32_t MaxUs = 0;

  for (int Idx = 0; Idx < Frames; Idx++) {
    uint32_t Sample = State->WindowFrameUs[Idx];
    SumUs += Sample;
    if (Sample > MaxUs) {
      MaxUs = Sample;
    }
    PushTopK(Top, TopCapacity, &TopCount, Sample);
  }

  uint64_t SumTop = 0;
  for (int Idx = 0; Idx < TopCount; Idx++) {
    SumTop += Top[Idx];
  }

  Out->AverageUs = (uint32_t)(SumUs / (uint64_t)Frames);
  Out->MaxUs = MaxUs;
  Out->OnePercentLowUs =
      (TopCount > 0) ? (uint32_t)(SumTop / (uint64_t)TopCount) : 0U;
}

// One definition of the row layout, shared by the window dump and the streaming
// capture, so the two can never drift into producing different columns.
static void WriteCsvHeader(FILE *FileVal) {
  (void)fprintf(FileVal, "frame,frame us");
  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    const char *Name = GetProfileScopeName((ProfileScope)Scope);
    (void)fprintf(FileVal, ",%s us,%s n", Name, Name);
  }
  (void)fprintf(FileVal, "\n");
}

static void WriteCsvRow(FILE *FileVal, const ProfilerState *State, int Row,
                        int Slot) {
  (void)fprintf(FileVal, "%d,%u", Row, State->WindowFrameUs[Slot]);
  for (int Scope = 0; Scope < PROFILE_SCOPE_COUNT; Scope++) {
    (void)fprintf(FileVal, ",%u,%u", State->WindowUs[Scope][Slot],
                  State->WindowEntries[Scope][Slot]);
  }
  (void)fprintf(FileVal, "\n");
}

// The oldest sample is at slot 0 until the ring wraps, and at the write cursor
// afterwards — the cursor points at the slot about to be overwritten, which is
// the oldest one still held. Walking forward modulo the window covers both: the
// unwrapped case starts at 0 and never reaches the wrap.
static int OldestSlot(const ProfilerState *State) {
  return (State->RecordedFrames < PROFILER_WINDOW_FRAMES) ? 0
                                                          : State->WriteCursor;
}

static bool OpenNextDumpFile(char *OutPath, size_t PathCapacity, FILE **OutFile) {
  enum { MAX_DUMP_INDEX = 1000 };

  for (int Index = 0; Index < MAX_DUMP_INDEX; Index++) {
    (void)snprintf(OutPath, PathCapacity, "profile_%03d.csv", Index);

    // Probe rather than overwrite. The workflow this exists for is capturing
    // twice on another machine and comparing; destroying the first capture with
    // the second would be the costliest way this could fail.
    FILE *Existing = fopen(OutPath, "r");
    if (Existing != NULL) {
      (void)fclose(Existing);
      continue;
    }

    FILE *FileVal = fopen(OutPath, "w");
    if (FileVal == NULL) {
      return false;
    }
    *OutFile = FileVal;
    return true;
  }

  return false;
}

ProfilerDumpResult ProfilerDumpCsv(char *OutPath, size_t PathCapacity) {
  if (OutPath == NULL || PathCapacity == 0) {
    return PROFILER_DUMP_WRITE_FAILED;
  }

  const ProfilerState *State = GetProfilerState();
  int Frames = State->RecordedFrames;
  if (Frames <= 0) {
    return PROFILER_DUMP_NO_SAMPLES;
  }

  FILE *FileVal = NULL;
  if (!OpenNextDumpFile(OutPath, PathCapacity, &FileVal)) {
    return PROFILER_DUMP_WRITE_FAILED;
  }

  WriteCsvHeader(FileVal);

  int Start = OldestSlot(State);
  for (int Row = 0; Row < Frames; Row++) {
    WriteCsvRow(FileVal, State, Row, (Start + Row) % PROFILER_WINDOW_FRAMES);
  }

  // A write error only surfaces reliably at close on a buffered stream, so the
  // result is decided there rather than from the fprintf calls above.
  bool HadError = (ferror(FileVal) != 0);
  if (fclose(FileVal) != 0 || HadError) {
    return PROFILER_DUMP_WRITE_FAILED;
  }

  return PROFILER_DUMP_OK;
}

bool ProfilerIsCapturing(void) {
  return GetProfilerState()->CaptureFile != NULL;
}

bool ProfilerStartCapture(char *OutPath, size_t PathCapacity) {
  if (OutPath == NULL || PathCapacity == 0) {
    return false;
  }

  ProfilerState *State = GetProfilerState();
  if (State->CaptureFile != NULL) {
    return false;
  }

  FILE *FileVal = NULL;
  if (!OpenNextDumpFile(OutPath, PathCapacity, &FileVal)) {
    return false;
  }

  WriteCsvHeader(FileVal);
  State->CaptureFile = FileVal;
  State->CaptureRow = 0;
  return true;
}

void ProfilerStopCapture(void) {
  ProfilerState *State = GetProfilerState();
  if (State->CaptureFile == NULL) {
    return;
  }
  (void)fclose(State->CaptureFile);
  State->CaptureFile = NULL;
  State->CaptureRow = 0;
}

#endif
