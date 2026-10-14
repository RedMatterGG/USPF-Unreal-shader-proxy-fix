// Crash diagnostics: logs fatal-looking exceptions (first chance) with module+offset and a
// short call stack, so a crash in a game without its own logs can still be attributed.
#include "common.h"

#include <psapi.h>

namespace d3a {
namespace {
std::atomic<int> g_logged{0};
// Copy-protection layers raise exceptions on purpose, often from the same few addresses:
// log each faulting address once so a real crash isn't crowded out.
std::atomic<void*> g_seen[256];
bool FirstTime(void* a) {
    for (auto& s : g_seen) {
        void* cur = s.load();
        if (cur == a) return false;
        if (!cur) {
            void* expected = nullptr;
            if (s.compare_exchange_strong(expected, a)) return true;
            if (expected == a) return false;
        }
    }
    return false;  // table full
}

}  // namespace
void DescribeAddress(void* addr, char* out, size_t n) {
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addr), &m) && m) {
        wchar_t path[MAX_PATH];
        DWORD len = GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < len; ++i)
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        snprintf(out, n, "%ls+0x%llx", name, (unsigned long long)((uintptr_t)addr - (uintptr_t)m));
    } else {
        snprintf(out, n, "%p", addr);
    }
}
namespace {
void Describe(void* addr, char* out, size_t n) { DescribeAddress(addr, out, n); }

LONG CALLBACK Handler(PEXCEPTION_POINTERS ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case 0xC0000409:  // fast-fail / stack buffer overrun
        case 0xC0000374:  // heap corruption
            break;
        // Routine, high-volume codes that say nothing about a crash:
        case 0x406D1388:  // thread naming
        case 0xE06D7363:  // C++ throw (caught normally)
        case 0x40010006:  // OutputDebugStringA
        case 0x4001000A:  // OutputDebugStringW
        case 0x000006BA:  // RPC server unavailable
        case 0x000006A6:  // RPC invalid binding
        case 0xE0434352:  // .NET exception
            return EXCEPTION_CONTINUE_SEARCH;
        default:
            break;  // anything else (engine fatal errors, breakpoints, custom codes): log it
    }
    if (!FirstTime(ep->ExceptionRecord->ExceptionAddress) || g_logged.fetch_add(1) >= 200) return EXCEPTION_CONTINUE_SEARCH;
    char where[400];
    Describe(ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    char extra[96] = "";
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        snprintf(extra, sizeof extra, " (%s 0x%llx)",
                 ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "execute" : ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                 (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    Log(LogLevel::Error, "exception 0x%08lx at %s%s (first chance; may be handled)", code, where, extra);
    // Stack from the faulting frame, walked with the unwind tables.
    CONTEXT ctx = *ep->ContextRecord;
    if (!ctx.Rip && ctx.Rsp && !IsBadReadPtr((void*)ctx.Rsp, 8)) {  // called a null pointer: start from the caller
        ctx.Rip = *(DWORD64*)ctx.Rsp;
        ctx.Rsp += 8;
        Log(LogLevel::Error, "    (null call; stack below starts at the caller)");
    }
    for (int i = 0; i < 16 && ctx.Rip; ++i) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
        char f[400];
        Describe((void*)ctx.Rip, f, sizeof f);
        Log(LogLevel::Error, "    #%d %s", i, f);
        if (!fn) {  // leaf function: return address is at [rsp]
            if (!ctx.Rsp || IsBadReadPtr((void*)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData;
        DWORD64 frame;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &frame, nullptr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void InstallCrashLog() { AddVectoredExceptionHandler(1, Handler); }

}  // namespace d3a
