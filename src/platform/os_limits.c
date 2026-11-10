#include "platform/os_limits.h"

#include "core/log.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>

static HANDLE *GetMemoryJobHandle(void) {
  static HANDLE JobHandle = (HANDLE)0;
  return &JobHandle;
}

static int SelectLogicalProcessorsMask(DWORD_PTR ProcessMask, int CoreCount,
                                       DWORD_PTR *OutTargetMask) {
  DWORD_PTR Remaining = ProcessMask;
  DWORD_PTR TargetMask = 0;
  int Available = 0;
  int Selected = 0;

  while (Remaining != 0) {
    DWORD_PTR LowestBit = Remaining & (~Remaining + (DWORD_PTR)1);
    Remaining ^= LowestBit;
    Available++;
    if (Selected < CoreCount) {
      TargetMask |= LowestBit;
      Selected++;
    }
  }

  *OutTargetMask = TargetMask;
  return Available;
}

static bool QueryPhysicalCoresMask(DWORD_PTR ProcessMask, int CoreCount,
                                   DWORD_PTR *OutTargetMask,
                                   int *OutAvailableCores) {
  DWORD BufferBytes = 0;
  (void)GetLogicalProcessorInformationEx(
      RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)0,
      &BufferBytes);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || BufferBytes == 0) {
    return false;
  }

  unsigned char *Buffer = (unsigned char *)malloc(BufferBytes);
  if (Buffer == (unsigned char *)0) {
    return false;
  }

  if (!GetLogicalProcessorInformationEx(
          RelationProcessorCore,
          (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Buffer, &BufferBytes)) {
    free(Buffer);
    return false;
  }

  DWORD_PTR TargetMask = 0;
  int AvailableCores = 0;
  int SelectedCores = 0;
  DWORD Offset = 0;

  while (Offset < BufferBytes) {
    const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *Info =
        (const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *)(Buffer + Offset);
    if (Info->Size == 0 || Offset + Info->Size > BufferBytes) {
      break;
    }

    if (Info->Relationship == RelationProcessorCore) {
      for (WORD GroupIdx = 0; GroupIdx < Info->Processor.GroupCount;
           GroupIdx++) {
        if (Info->Processor.GroupMask[GroupIdx].Group != 0) {
          continue;
        }
        KAFFINITY CoreMask =
            Info->Processor.GroupMask[GroupIdx].Mask & ProcessMask;
        if (CoreMask != 0) {
          KAFFINITY LowestBit = CoreMask & (~CoreMask + (KAFFINITY)1);
          AvailableCores++;
          if (SelectedCores < CoreCount) {
            TargetMask |= LowestBit;
            SelectedCores++;
          }
          break;
        }
      }
    }

    Offset += Info->Size;
  }

  free(Buffer);
  if (AvailableCores == 0 || TargetMask == 0) {
    return false;
  }

  *OutTargetMask = TargetMask;
  *OutAvailableCores = AvailableCores;
  return true;
}

bool OsLimitPhysicalCores(int CoreCount, int *OutApplied) {
  if (CoreCount <= 0) {
    return false;
  }

  DWORD_PTR ProcessMask = 0;
  DWORD_PTR SystemMask = 0;
  if (!GetProcessAffinityMask(GetCurrentProcess(), &ProcessMask, &SystemMask) ||
      ProcessMask == 0) {
    LogError("OS: GetProcessAffinityMask failed (error %lu)", GetLastError());
    return false;
  }

  DWORD_PTR TargetMask = 0;
  int AvailableCores = 0;
  if (!QueryPhysicalCoresMask(ProcessMask, CoreCount, &TargetMask,
                              &AvailableCores)) {
    LogWarn("OS: physical core topology unavailable; falling back to logical "
            "processors (SMT siblings may be counted)");
    AvailableCores =
        SelectLogicalProcessorsMask(ProcessMask, CoreCount, &TargetMask);
  }

  if (AvailableCores <= 0 || TargetMask == 0) {
    LogError("OS: no usable processors found in affinity mask");
    return false;
  }

  int Applied = CoreCount;
  if (Applied > AvailableCores) {
    LogWarn("OS: requested %d cores, clamping to %d available", CoreCount,
            AvailableCores);
    Applied = AvailableCores;
  }

  if (!SetProcessAffinityMask(GetCurrentProcess(), TargetMask)) {
    LogError("OS: SetProcessAffinityMask failed (error %lu)", GetLastError());
    return false;
  }

  if (OutApplied != (int *)0) {
    *OutApplied = Applied;
  }
  return true;
}

bool OsLimitProcessMemory(size_t Bytes) {
  if (Bytes == 0) {
    return false;
  }

  HANDLE Job = CreateJobObjectA((LPSECURITY_ATTRIBUTES)0, (LPCSTR)0);
  if (Job == (HANDLE)0) {
    LogError("OS: CreateJobObject failed (error %lu)", GetLastError());
    return false;
  }

  JOBOBJECT_EXTENDED_LIMIT_INFORMATION ExtInfo = {0};
  ExtInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY;
  ExtInfo.ProcessMemoryLimit = Bytes;

  if (!SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &ExtInfo,
                               sizeof(ExtInfo))) {
    LogError("OS: SetInformationJobObject failed (error %lu)", GetLastError());
    (void)CloseHandle(Job);
    return false;
  }

  if (!AssignProcessToJobObject(Job, GetCurrentProcess())) {
    LogError("OS: AssignProcessToJobObject failed (error %lu)", GetLastError());
    (void)CloseHandle(Job);
    return false;
  }

  *GetMemoryJobHandle() = Job;
  return true;
}

size_t OsGetPeakProcessMemory(void) {
  HANDLE Job = *GetMemoryJobHandle();
  if (Job != (HANDLE)0) {
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION ExtInfo = {0};
    if (QueryInformationJobObject(Job, JobObjectExtendedLimitInformation,
                                  &ExtInfo, sizeof(ExtInfo), (LPDWORD)0) &&
        ExtInfo.PeakProcessMemoryUsed > 0) {
      return ExtInfo.PeakProcessMemoryUsed;
    }
  }

  PROCESS_MEMORY_COUNTERS Counters = {0};
  Counters.cb = sizeof(Counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &Counters, sizeof(Counters))) {
    return Counters.PeakPagefileUsage;
  }
  return 0;
}

const char *OsGetPeakProcessMemoryKind(void) { return "committed"; }

#else

#include <sched.h>
#include <stdio.h>
#include <sys/resource.h>

#define BYTES_PER_KILOBYTE 1024U

enum {
  SYSFS_PATH_CAPACITY = 128,
  SYSFS_LINE_CAPACITY = 32,
  DECIMAL_BASE = 10
};

typedef struct {
  int PackageId;
  int CoreId;
  int RepresentativeCpu;
} PhysicalCoreEntry;

static bool ReadSysfsInt(const char *Path, int *OutVal) {
  FILE *File = fopen(Path, "r");
  if (File == (FILE *)0) {
    return false;
  }
  char Line[SYSFS_LINE_CAPACITY];
  bool ReadOk = (fgets(Line, sizeof(Line), File) != (char *)0);
  (void)fclose(File);
  if (!ReadOk) {
    return false;
  }
  char *End = (char *)0;
  long Value = strtol(Line, &End, DECIMAL_BASE);
  if (End == Line) {
    return false;
  }
  *OutVal = (int)Value;
  return true;
}

static bool ReadCpuTopologyIds(int CpuIndex, int *OutPackageId,
                               int *OutCoreId) {
  char PackagePath[SYSFS_PATH_CAPACITY];
  char CorePath[SYSFS_PATH_CAPACITY];
  (void)snprintf(
      PackagePath, sizeof(PackagePath),
      "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", CpuIndex);
  (void)snprintf(CorePath, sizeof(CorePath),
                 "/sys/devices/system/cpu/cpu%d/topology/core_id", CpuIndex);
  return ReadSysfsInt(PackagePath, OutPackageId) &&
         ReadSysfsInt(CorePath, OutCoreId);
}

static bool CollectPhysicalCoresLinux(const cpu_set_t *AllowedSet,
                                      PhysicalCoreEntry *Entries,
                                      int MaxEntries, int *OutCoreCount) {
  int Count = 0;
  for (int Cpu = 0; Cpu < CPU_SETSIZE; Cpu++) {
    if (!CPU_ISSET(Cpu, AllowedSet)) {
      continue;
    }

    int PackageId = 0;
    int CoreId = 0;
    if (!ReadCpuTopologyIds(Cpu, &PackageId, &CoreId)) {
      return false;
    }

    bool AlreadySeen = false;
    for (int Idx = 0; Idx < Count; Idx++) {
      if (Entries[Idx].PackageId == PackageId &&
          Entries[Idx].CoreId == CoreId) {
        AlreadySeen = true;
        break;
      }
    }

    if (!AlreadySeen && Count < MaxEntries) {
      Entries[Count].PackageId = PackageId;
      Entries[Count].CoreId = CoreId;
      Entries[Count].RepresentativeCpu = Cpu;
      Count++;
    }
  }

  if (Count == 0) {
    return false;
  }
  *OutCoreCount = Count;
  return true;
}

bool OsLimitPhysicalCores(int CoreCount, int *OutApplied) {
  if (CoreCount <= 0) {
    return false;
  }

  cpu_set_t AllowedSet;
  CPU_ZERO(&AllowedSet);
  if (sched_getaffinity(0, sizeof(AllowedSet), &AllowedSet) != 0) {
    LogError("OS: sched_getaffinity failed");
    return false;
  }

  PhysicalCoreEntry Cores[CPU_SETSIZE];
  int AvailableCores = 0;
  cpu_set_t TargetSet;
  CPU_ZERO(&TargetSet);

  if (CollectPhysicalCoresLinux(&AllowedSet, Cores, CPU_SETSIZE,
                                &AvailableCores)) {
    int Selected =
        (CoreCount < AvailableCores) ? CoreCount : AvailableCores;
    for (int Idx = 0; Idx < Selected; Idx++) {
      CPU_SET(Cores[Idx].RepresentativeCpu, &TargetSet);
    }
  } else {
    LogWarn("OS: physical core topology unavailable; falling back to logical "
            "processors (SMT siblings may be counted)");
    int Selected = 0;
    for (int Cpu = 0; Cpu < CPU_SETSIZE; Cpu++) {
      if (!CPU_ISSET(Cpu, &AllowedSet)) {
        continue;
      }
      AvailableCores++;
      if (Selected < CoreCount) {
        CPU_SET(Cpu, &TargetSet);
        Selected++;
      }
    }
  }

  if (AvailableCores <= 0) {
    LogError("OS: no usable processors found in affinity mask");
    return false;
  }

  int Applied = CoreCount;
  if (Applied > AvailableCores) {
    LogWarn("OS: requested %d cores, clamping to %d available", CoreCount,
            AvailableCores);
    Applied = AvailableCores;
  }

  if (sched_setaffinity(0, sizeof(TargetSet), &TargetSet) != 0) {
    LogError("OS: sched_setaffinity failed");
    return false;
  }

  if (OutApplied != (int *)0) {
    *OutApplied = Applied;
  }
  return true;
}

bool OsLimitProcessMemory(size_t Bytes) {
  if (Bytes == 0) {
    return false;
  }

  struct rlimit Limit = {0};
  if (getrlimit(RLIMIT_DATA, &Limit) != 0) {
    LogError("OS: getrlimit(RLIMIT_DATA) failed");
    return false;
  }

  rlim_t Target = (rlim_t)Bytes;
  if (Limit.rlim_max != RLIM_INFINITY && Target > Limit.rlim_max) {
    Target = Limit.rlim_max;
  }
  Limit.rlim_cur = Target;

  if (setrlimit(RLIMIT_DATA, &Limit) != 0) {
    LogError("OS: setrlimit(RLIMIT_DATA) failed");
    return false;
  }
  return true;
}

size_t OsGetPeakProcessMemory(void) {
  struct rusage Usage = {0};
  if (getrusage(RUSAGE_SELF, &Usage) != 0 || Usage.ru_maxrss <= 0) {
    return 0;
  }
  return (size_t)Usage.ru_maxrss * BYTES_PER_KILOBYTE;
}

const char *OsGetPeakProcessMemoryKind(void) { return "resident"; }

#endif
