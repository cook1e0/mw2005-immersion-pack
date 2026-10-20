// Patching speed.exe: checks that its code and data are what the mod expects, and writes to them.
#pragma once
#include "common.h"
#include <initializer_list>
#include <string.h>

// The bytes at `at` are `bytes`; logs the first one that isn't.
inline bool Expect(uintptr_t at, std::initializer_list<uint8_t> bytes) {
    const uint8_t* p = (const uint8_t*)at;
    size_t i = 0;
    for (uint8_t b : bytes) {
        if (p[i] != b) {
            Log("unexpected code at 0x%06X+%u: %02X (want %02X)", (unsigned)at, (unsigned)i, p[i], b);
            return false;
        }
        i++;
    }
    return true;
}

// The pointer at `at` (a vtable slot) is `value`; logs it if not.
inline bool ExpectPtr(uintptr_t at, uintptr_t value) {
    if (*(uintptr_t*)at == value) return true;
    Log("unexpected data at 0x%06X: %08X (want %08X)", (unsigned)at, (unsigned)*(uintptr_t*)at, (unsigned)value);
    return false;
}

inline void Write(uintptr_t at, const void* data, size_t n) {
    DWORD old;
    VirtualProtect((void*)at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy((void*)at, data, n);
    VirtualProtect((void*)at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, n);
}

// A call (op 0xE8) or jmp (0xE9) at `at` to `target`.
inline void WriteRel(uintptr_t at, uint8_t op, const void* target) {
    uint8_t code[5] = {op};
    int32_t rel = (int32_t)((uintptr_t)target - (at + 5));
    memcpy(code + 1, &rel, 4);
    Write(at, code, 5);
}

// `len` bytes of whole instructions at `at` replaced by a call to `target` and nops after it.
inline void WriteCallNops(uintptr_t at, size_t len, const void* target) {
    uint8_t code[16];
    code[0] = 0xE8;
    *(int32_t*)(code + 1) = (int32_t)((uintptr_t)target - (at + 5));
    memset(code + 5, 0x90, len - 5);
    Write(at, code, len);
}

// The call at `site` goes to `hook`, if it still calls `target` (silently false if not).
inline bool RetargetCall(uintptr_t site, uintptr_t target, const void* hook) {
    if (*(uint8_t*)site != 0xE8 || site + 5 + *(int32_t*)(site + 1) != target) return false;
    WriteRel(site, 0xE8, hook);
    return true;
}

// The vtable slot `slot` points to `hook`, if it still points to `expected` (silently false if not).
inline bool SetSlot(uintptr_t slot, uintptr_t expected, const void* hook) {
    if (*(uintptr_t*)slot != expected) return false;
    Write(slot, &hook, 4);
    return true;
}

// Jumps from `at` to `hook`; returns a trampoline running the `stolen` bytes (whole instructions
// that don't depend on where they are) and jumping back, or null.
inline void* Detour(uintptr_t at, size_t stolen, const void* hook) {
    uint8_t* tramp = (uint8_t*)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, (void*)at, stolen);
    tramp[stolen] = 0xE9;
    *(int32_t*)(tramp + stolen + 1) = (int32_t)((at + stolen) - ((uintptr_t)tramp + stolen + 5));
    uint8_t code[16];
    code[0] = 0xE9;
    *(int32_t*)(code + 1) = (int32_t)((uintptr_t)hook - (at + 5));
    memset(code + 5, 0x90, stolen - 5);
    Write(at, code, stolen);
    return tramp;
}
