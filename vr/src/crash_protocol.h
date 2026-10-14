// Both ends are x86: the game publishes a copy of its exception to a dump helper.
#pragma once
#include "common.h"

#define CRASH_HELPER_EXE "nfsmw_crashdump.exe"
#define CRASH_MAPPING "Local\\nfsmw_crash_%lu"
#define CRASH_REQUEST "Local\\nfsmw_crash_%lu_req"
#define CRASH_REPLY "Local\\nfsmw_crash_%lu_rep"
constexpr DWORD kCrashProtocolVersion = 1;

struct CrashShared {
    DWORD version;
    DWORD threadId;
    DWORD error;
    BOOL ready;
    BOOL success;
    EXCEPTION_RECORD record;
    CONTEXT context;
    char dumpPath[MAX_PATH];
};
static_assert(sizeof(void*) == 4, "crash helper and game must both be x86");
