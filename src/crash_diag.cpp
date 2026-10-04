// Last-chance crash logger: writes faulting address, operation, and a
// symbolized stack to crash_diag.log. Build RelWithDebInfo for resolved names.

#include <windows.h>

#include <dbghelp.h>

#include <cstdint>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace {

void SymLine(std::FILE* f, HANDLE proc, DWORD64 addr, int idx) {
  char buf[sizeof(SYMBOL_INFO) + 512];
  auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
  si->SizeOfStruct = sizeof(SYMBOL_INFO);
  si->MaxNameLen = 511;
  IMAGEHLP_MODULE64 mod;
  mod.SizeOfStruct = sizeof(mod);
  const char* modname =
      SymGetModuleInfo64(proc, addr, &mod) ? mod.ModuleName : "?";
  DWORD64 disp = 0;
  if (SymFromAddr(proc, addr, &disp, si)) {
    std::fprintf(f, "  [%2d] %s!%s +0x%llX  (0x%llX)\n", idx, modname, si->Name,
                 (unsigned long long)disp, (unsigned long long)addr);
  } else {
    std::fprintf(f, "  [%2d] %s!0x%llX\n", idx, modname,
                 (unsigned long long)addr);
  }
  IMAGEHLP_LINE64 line;
  line.SizeOfStruct = sizeof(line);
  DWORD line_disp = 0;
  if (SymGetLineFromAddr64(proc, addr, &line_disp, &line)) {
    std::fprintf(f, "         %s:%lu\n", line.FileName, line.LineNumber);
  }
}

LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
  auto* er = ep->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  std::FILE* f = std::fopen("crash_diag.log", "a");
  if (!f) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  HANDLE proc = GetCurrentProcess();
  static bool inited = false;
  if (!inited) {
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(proc, nullptr, TRUE);
    inited = true;
  }

  const char* op = er->ExceptionInformation[0] == 1   ? "write"
                   : er->ExceptionInformation[0] == 0 ? "read"
                                                      : "exec";
  CONTEXT* ctx = ep->ContextRecord;
  std::fprintf(f, "\n==== ACCESS VIOLATION (%s 0x%llX) thread %lu ====\n", op,
               (unsigned long long)er->ExceptionInformation[1],
               GetCurrentThreadId());
  std::fprintf(f, "faulting instruction:\n");
  SymLine(f, proc, ctx->Rip, 0);

  std::fprintf(f, "stack:\n");
  STACKFRAME64 sf{};
  sf.AddrPC.Offset = ctx->Rip;
  sf.AddrPC.Mode = AddrModeFlat;
  sf.AddrFrame.Offset = ctx->Rbp;
  sf.AddrFrame.Mode = AddrModeFlat;
  sf.AddrStack.Offset = ctx->Rsp;
  sf.AddrStack.Mode = AddrModeFlat;
  CONTEXT walk = *ctx;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf,
                     &walk, nullptr, SymFunctionTableAccess64,
                     SymGetModuleBase64, nullptr)) {
      break;
    }
    if (!sf.AddrPC.Offset) {
      break;
    }
    SymLine(f, proc, sf.AddrPC.Offset, i);
  }
  std::fflush(f);
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

struct Install {
  Install() { AddVectoredExceptionHandler(0, Veh); }
} g_install;

}  // namespace
