// Integration harness for the production capture code; never loads the game.
#include "../src/crash.h"
#include <dbghelp.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int Verify(const char* path) {
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
    HANDLE mapping = CreateFileMappingA(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    void* base = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    PMINIDUMP_DIRECTORY directory;
    void* stream = nullptr;
    ULONG size = 0;
    bool ok = base && MiniDumpReadDumpStream(base, ExceptionStream, &directory, &stream, &size);
    auto* exception = (MINIDUMP_EXCEPTION_STREAM*)stream;
    ok = ok && size >= sizeof(*exception) && exception->ThreadId &&
         exception->ExceptionRecord.ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
         exception->ExceptionRecord.NumberParameters == 2 &&
         exception->ExceptionRecord.ExceptionInformation[0] == 1 &&
         exception->ExceptionRecord.ExceptionInformation[1] == 0 &&
         exception->ThreadContext.DataSize == sizeof(CONTEXT);
    if (ok) {
        auto* context = (CONTEXT*)((char*)base + exception->ThreadContext.Rva);
        ok = context->Eip != 0 && context->Esp != 0;
    }
    stream = nullptr;
    ok = ok && MiniDumpReadDumpStream(base, ModuleListStream, &directory, &stream, &size) &&
         ((MINIDUMP_MODULE_LIST*)stream)->NumberOfModules > 0;
    stream = nullptr;
    ok = ok && MiniDumpReadDumpStream(base, ThreadListStream, &directory, &stream, &size) &&
         ((MINIDUMP_THREAD_LIST*)stream)->NumberOfThreads > 0;
    if (base) UnmapViewOfFile(base);
    if (mapping) CloseHandle(mapping);
    CloseHandle(file);
    printf("dump exception, x86 context, module list and thread list: %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 3 && strcmp(argv[1], "verify") == 0) return Verify(argv[2]);
    if (argc != 2) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    LogInit();
    g_cfg.crashDumps = strcmp(argv[1], "disabled") != 0;
    Crash_Initialize();
    // Initialisation must be idempotent.
    Crash_Initialize();
    if (strcmp(argv[1], "handled") == 0) {
        __try {
            RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }
    if (strcmp(argv[1], "normal") == 0) return 0;
    if (strcmp(argv[1], "fatal") == 0) {
        // A native bad-memory write verifies capture of the faulting instruction,
        // rather than just the context inside RaiseException.
        volatile DWORD* bad = (volatile DWORD*)strtoul("0", nullptr, 10);
        *bad = 42;
    }
    ULONG_PTR access[] = {1, 0};
    RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 2, access);
    return 1;
}
