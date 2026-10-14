#include "crash.h"
#include "crash_protocol.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>

namespace {
CrashShared* s_shared;
HANDLE s_mapping, s_request, s_reply, s_helper;
LPTOP_LEVEL_EXCEPTION_FILTER s_previous;
volatile LONG s_capturing;

// No allocations, logging locks or DbgHelp calls in the crashing process.
LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep) {
    if (s_shared && ep && ep->ExceptionRecord && ep->ContextRecord) {
        if (InterlockedCompareExchange(&s_capturing, 1, 0) == 0) {
            s_shared->threadId = GetCurrentThreadId();
            s_shared->record = *ep->ExceptionRecord;
            s_shared->record.ExceptionRecord = nullptr;
            s_shared->context = *ep->ContextRecord;
            SetEvent(s_request);
        }
        // Simultaneous crashing threads wait for the same dump instead of ending the
        // process while the first thread's exception is still being captured.
        HANDLE wait[] = {s_reply, s_helper};
        WaitForMultipleObjects(2, wait, FALSE, 15000);
    }
    return s_previous ? s_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void CloseSetup() {
    if (s_shared) UnmapViewOfFile(s_shared);
    s_shared = nullptr;
    for (HANDLE* h : {&s_mapping, &s_request, &s_reply, &s_helper}) {
        if (*h) CloseHandle(*h);
        *h = nullptr;
    }
}
}

void Crash_Initialize() {
    static bool attempted;
    if (attempted) return;
    attempted = true;
    if (!g_cfg.crashDumps) {
        Log("crash: automatic dumps disabled (CrashDumps=0)");
        return;
    }
    char exe[MAX_PATH], command[MAX_PATH + 48], name[64];
    DWORD pid = GetCurrentProcessId();
    if (snprintf(exe, sizeof(exe), "%s%s", GameDir(), CRASH_HELPER_EXE) >= (int)sizeof(exe)) {
        Log("crash: helper path is too long; automatic dumps unavailable");
        return;
    }
    snprintf(name, sizeof(name), CRASH_MAPPING, pid);
    s_mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(CrashShared), name);
    s_shared = s_mapping ? (CrashShared*)MapViewOfFile(s_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrashShared)) : nullptr;
    snprintf(name, sizeof(name), CRASH_REQUEST, pid);
    s_request = CreateEventA(nullptr, FALSE, FALSE, name);
    snprintf(name, sizeof(name), CRASH_REPLY, pid);
    s_reply = CreateEventA(nullptr, TRUE, FALSE, name);
    if (!s_shared || !s_request || !s_reply) {
        Log("crash: shared-memory setup failed (%lu); automatic dumps unavailable", GetLastError());
        CloseSetup();
        return;
    }
    memset(s_shared, 0, sizeof(*s_shared));
    s_shared->version = kCrashProtocolVersion;
    snprintf(command, sizeof(command), "\"%s\" %lu", exe, pid);
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(exe, command, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, GameDir(), &startup, &process)) {
        Log("crash: cannot start %s (%lu); install it next to speed.exe for automatic dumps", CRASH_HELPER_EXE, GetLastError());
        CloseSetup();
        return;
    }
    CloseHandle(process.hThread);
    s_helper = process.hProcess;
    HANDLE wait[] = {s_reply, s_helper};
    if (WaitForMultipleObjects(2, wait, FALSE, 5000) != WAIT_OBJECT_0 || !s_shared->ready) {
        Log("crash: dump helper did not become ready; automatic dumps unavailable");
        // Wake a late helper so it can exit instead of remaining idle until game shutdown.
        SetEvent(s_request);
        CloseSetup();
        return;
    }
    ResetEvent(s_reply);
    s_previous = SetUnhandledExceptionFilter(OnUnhandled);
    Log("crash: automatic dumps ready; an unhandled crash saves nfsmw_crash.dmp and nfsmw_crash.txt next to speed.exe");
}
