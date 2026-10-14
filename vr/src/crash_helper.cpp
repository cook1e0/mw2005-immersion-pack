// Prestarted x86 helper: writes diagnostics outside the damaged game process.
#include "crash_protocol.h"
#include <dbghelp.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
void WriteReport(HANDLE game, const CrashShared* shared) {
    char report[MAX_PATH];
    strcpy_s(report, shared->dumpPath);
    char* extension = strrchr(report, '.');
    if (!extension) return;
    strcpy_s(extension, sizeof(report) - (extension - report), ".txt");
    FILE* out = fopen(report, "w");
    if (!out) return;
    fprintf(out, "NFSMW crash diagnostics\nDump: %s\nDump written: %s (error 0x%08lx)\n",
            shared->dumpPath, shared->success ? "yes" : "no", shared->error);
    const auto& record = shared->record;
    fprintf(out, "Exception: 0x%08lx\nThread: %lu\nAddress: %p\n", record.ExceptionCode,
            shared->threadId, record.ExceptionAddress);
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQueryEx(game, record.ExceptionAddress, &memory, sizeof(memory))) {
        char module[MAX_PATH] = {};
        if (GetModuleFileNameExA(game, (HMODULE)memory.AllocationBase, module, sizeof(module)))
            fprintf(out, "Module: %s\nModule base: %p\nFault offset: 0x%08lx\n", module,
                    memory.AllocationBase, (DWORD)((uintptr_t)record.ExceptionAddress - (uintptr_t)memory.AllocationBase));
    }
    if ((record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record.ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record.NumberParameters >= 2)
        fprintf(out, "Access: %s\nAccess address: 0x%08lx\n",
                record.ExceptionInformation[0] == 0 ? "read" : record.ExceptionInformation[0] == 1 ? "write" : "execute",
                (DWORD)record.ExceptionInformation[1]);
    fprintf(out, "\nSend this file, the .dmp, nfsmw_vr.log and nfsmw_vr_host.log (if present).\n");
    fclose(out);
}

void WriteDump(HANDLE game, DWORD pid, CrashShared* shared) {
    int length = snprintf(shared->dumpPath, sizeof(shared->dumpPath), "%snfsmw_crash.dmp", GameDir());
    HANDLE file = INVALID_HANDLE_VALUE;
    if (length >= 0 && length < (int)sizeof(shared->dumpPath))
        file = CreateFileA(shared->dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    else SetLastError(ERROR_FILENAME_EXCED_RANGE);
    if (file == INVALID_HANDLE_VALUE) {
        // Installed games can live in a folder where the user cannot write.
        char temp[MAX_PATH];
        DWORD tempLength = GetTempPathA(sizeof(temp), temp);
        if (tempLength > 0 && tempLength < sizeof(temp)) {
            length = snprintf(shared->dumpPath, sizeof(shared->dumpPath), "%snfsmw_crash_%lu.dmp", temp, pid);
            if (length >= 0 && length < (int)sizeof(shared->dumpPath))
                file = CreateFileA(shared->dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            else SetLastError(ERROR_FILENAME_EXCED_RANGE);
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        shared->error = GetLastError();
        Log("crash: cannot create dump (%lu)", shared->error);
        return;
    }
    EXCEPTION_POINTERS pointers = {&shared->record, &shared->context};
    MINIDUMP_EXCEPTION_INFORMATION exception = {shared->threadId, &pointers, FALSE};
    auto type = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules |
                               MiniDumpWithProcessThreadData | MiniDumpWithFullMemoryInfo | MiniDumpWithThreadInfo);
    shared->success = MiniDumpWriteDump(game, pid, file, type, &exception, nullptr, nullptr);
    shared->error = shared->success ? 0 : GetLastError();
    CloseHandle(file);
    WriteReport(game, shared);
    Log("crash: dump %s: %s (error 0x%08lx)", shared->success ? "saved" : "failed", shared->dumpPath, shared->error);
}
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR command, int) {
    LogInit("nfsmw_crashdump.log");
    char* end = nullptr;
    DWORD pid = strtoul(command, &end, 10);
    if (!pid || end == command || *end) return 1;
    char name[64];
    snprintf(name, sizeof(name), CRASH_MAPPING, pid);
    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    auto* shared = mapping ? (CrashShared*)MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrashShared)) : nullptr;
    snprintf(name, sizeof(name), CRASH_REQUEST, pid);
    HANDLE request = OpenEventA(SYNCHRONIZE, FALSE, name);
    snprintf(name, sizeof(name), CRASH_REPLY, pid);
    HANDLE reply = OpenEventA(EVENT_MODIFY_STATE, FALSE, name);
    HANDLE game = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!shared || !request || !reply || !game || shared->version != kCrashProtocolVersion) {
        Log("crash: cannot attach to game %lu (%lu)", pid, GetLastError());
        return 1;
    }
    shared->ready = TRUE;
    SetEvent(reply);
    HANDLE wait[] = {request, game};
    DWORD result = WaitForMultipleObjects(2, wait, FALSE, INFINITE);
    if (result == WAIT_OBJECT_0 && shared->threadId) {
        WriteDump(game, pid, shared);
        SetEvent(reply);
    }
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    CloseHandle(request);
    CloseHandle(reply);
    CloseHandle(game);
    return 0;
}
