// Mass Effect - profile dump of the instrumented build for PGO.
//
// Only active with MASSEFFECT_PGO=generate (CMakeLists.txt). GCC embeds in the executable the path
// <MASSEFFECT_PGO_DIR>/<object path with # for />.gcda (the profile folder on the build machine); the fopen
// --wrap redirects it to sdmc:/switch/masseffect-nx/pgo/. The Switch does not always close the program cleanly
// (HOME kills it), so a thread dumps the counters every 60 s and resets them; libgcov adds to whatever each
// .gcda already holds, so several dumps give the total of the session.
#if defined(MASSEFFECT_PGO_GENERATE) && defined(__SWITCH__)

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

extern "C" {
FILE* __real_fopen(const char* path, const char* mode);
void __gcov_dump(void);
void __gcov_reset(void);

FILE* __wrap_fopen(const char* path, const char* mode) {
  static constexpr char kPrefix[] = MASSEFFECT_PGO_DIR;
  if (path && std::strncmp(path, kPrefix, sizeof(kPrefix) - 1) == 0) {
    const char* rest = path + sizeof(kPrefix) - 1;
    while (*rest == '\\' || *rest == '/') ++rest;
    std::string target = "sdmc:/switch/masseffect-nx/pgo/";
    for (; *rest; ++rest) target += (*rest == '\\') ? '/' : *rest;
    return __real_fopen(target.c_str(), mode);
  }
  return __real_fopen(path, mode);
}
}

namespace {
Thread g_pgo_thread;

void PgoThread(void*) {
  for (int dump = 1;; ++dump) {
    svcSleepThread(60LL * 1000 * 1000 * 1000);
    mkdir("sdmc:/switch/masseffect-nx/pgo", 0777);
    const u64 before = armGetSystemTick();
    __gcov_dump();
    __gcov_reset();
    const double ms = double(armTicksToNs(armGetSystemTick() - before)) / 1e6;
    if (FILE* f = __real_fopen("sdmc:/switch/masseffect-nx/pgo/dumps.txt", "a")) {
      std::fprintf(f, "dump %d: %.0f ms\n", dump, ms);
      std::fclose(f);
    }
  }
}

__attribute__((constructor)) void StartPgo() {
  if (R_SUCCEEDED(threadCreate(&g_pgo_thread, PgoThread, nullptr, nullptr, 64 * 1024, 0x3B, -2)))
    threadStart(&g_pgo_thread);
}
}  // namespace

#endif
