// Mass Effect for Nintendo Switch: packaged mode (installed full NSP). See me_packaged.h and docs/full-nsp.md.

#include "me_packaged.h"

#include <string>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#if defined(__SWITCH__)

#include <switch.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

extern "C" int __system_argc;
extern "C" char** __system_argv;

namespace {

// Written by the packer (tools/build_full_nsp.py). Plain "key=value" lines; only data_dir= is read here.
constexpr char kMarker[] = "romfs:/masseffect-nx-package.txt";
constexpr char kRomfsRoot[] = "romfs:/";
constexpr char kDefaultDataDir[] = "sdmc:/switch/masseffect-nx-nsp";
// A real file in the SD folder that argv[0] points at. GetExecutablePath (sdk/src/core/filesystem_posix.cpp) only
// turns "sdmc:/x" into the rooted "/x" when stat() finds it, and the unrooted form breaks std::filesystem::absolute.
constexpr char kAnchorName[] = "masseffect-nx-nsp.txt";
constexpr char kAnchorText[] =
    "Writable folder of the installed Mass Effect NSP (saves in masseffect/, cache/, logs/).\n"
    "The game data is inside the NSP. Put a masseffect.toml here to override the packaged settings.\n";

// All of this is set in userAppInit, which runs before the static constructors: only constant-initialized storage
// (no std::string), or the constructors would reset it afterwards.
bool g_active = false;
// What userAppInit saw, step by step. stderr only becomes rex_stderr.log in switch_crash_hooks.c's constructor
// (priority 101, after userAppInit), so the lines are kept here and written by FlushPackagedDiagnostics below.
char g_diag[1536] = "";
size_t g_diag_len = 0;

void Diag(const char* format, ...) __attribute__((format(printf, 1, 2)));
void Diag(const char* format, ...) {
  if (g_diag_len >= sizeof(g_diag) - 1) return;
  va_list args;
  va_start(args, format);
  const int n = std::vsnprintf(g_diag + g_diag_len, sizeof(g_diag) - g_diag_len, format, args);
  va_end(args);
  if (n > 0) g_diag_len = std::min(sizeof(g_diag) - 1, g_diag_len + size_t(n));
}
char g_data_dir[FS_MAX_PATH] = "";
char g_anchor[FS_MAX_PATH] = "";
char* g_argv[2] = {g_anchor, nullptr};

bool ValidDataDir(const char* s) {
  const size_t n = std::strlen(s);
  if (n <= 6 || n + sizeof(kAnchorName) + 2 >= FS_MAX_PATH) return false;
  if (std::strncmp(s, "sdmc:/", 6) != 0) return false;
  if (std::strstr(s, "..") || std::strstr(s, "//") || std::strchr(s + 6, ':')) return false;
  return true;
}

void ReadMarker() {
  std::FILE* f = std::fopen(kMarker, "r");
  if (!f) return;
  char line[FS_MAX_PATH + 32];
  while (std::fgets(line, sizeof(line), f)) {
    size_t n = std::strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '/')) line[--n] = 0;
    constexpr char kKey[] = "data_dir=";
    if (std::strncmp(line, kKey, sizeof(kKey) - 1) == 0 && ValidDataDir(line + sizeof(kKey) - 1)) {
      std::snprintf(g_data_dir, sizeof(g_data_dir), "%s", line + sizeof(kKey) - 1);
    }
  }
  std::fclose(f);
}

// mkdir -p for "sdmc:/a/b/c" (existing components fail harmlessly).
void MakeDirs(const char* dir) {
  char path[FS_MAX_PATH];
  std::snprintf(path, sizeof(path), "%s", dir);
  for (char* p = path + 6; *p; ++p) {
    if (*p == '/') {
      *p = 0;
      mkdir(path, 0777);
      *p = '/';
    }
  }
  mkdir(path, 0777);
}

}  // namespace

extern "C" char* fake_heap_start;
extern "C" char* fake_heap_end;

// The newlib heap, in both modes the one the NRO has always run with.
//
// NRO: hbloader passes its own heap (EntryType_OverrideHeap: all the application's memory minus 2 MB and the NRO
// image, Forwarder-Mod and nx-hbloader setupHbHeap), and libnx uses it as is; __nx_heap_size (runtime_switch.cpp)
// is never read in that case. This branch is libnx's own code, unchanged.
// NSO (installed NSP): there is no override, and libnx would apply __nx_heap_size (1 GiB), a configuration the game
// has never run with. The same size hbloader computes is taken instead, so both modes see the same memory.
extern "C" void __libnx_initheap(void) {
  void* addr = nullptr;
  size_t size = 0;
  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
    // hbloader's heap leaves only ~2 MB of the application pool to the kernel (pool 3281/3285 MB in use). Page
    // tables for new mappings, GPU and display buffer queues and file-system work then hit svc::ResultLimitReached
    // (0x10801) at start: refused display dequeues/queues, failed shader-package and game-package reads ("Disc Read
    // Error"), sys-ftpd unable to read its config (2026-10-10, mostly the EN edition). Give kRelease back.
    constexpr size_t kRelease = 0x4000000;  // 64 MB
    if (size > kRelease * 8) {
      void* shrunk = nullptr;
      if (R_SUCCEEDED(svcSetHeapSize(&shrunk, size - kRelease))) {
        addr = shrunk;  // the heap region does not move; take the kernel's answer anyway
        size -= kRelease;
      }
    }
  } else {
    u64 available = 0, used = 0;
    svcGetInfo(&available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (available > used + 0x200000) size = (available - used - 0x200000) & ~u64(0x1FFFFF);
    if (size == 0) size = 0x2000000 * 16;
    // hbloader leaves 96 MB for the system's automatic gameplay recording when the NACP enables it (VideoCapture 2),
    // which tools/build_nsp.sh and tools/build_full_nsp.py both set. (No services yet here to read the NACP.)
    if (size > 0x6000000) size -= 0x6000000;
    // The same 64 MB for the kernel as on the NRO path above.
    if (size > 0x4000000 * 8) size -= 0x4000000;
    if (R_FAILED(svcSetHeapSize(&addr, size))) diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }
  fake_heap_start = static_cast<char*>(addr);
  fake_heap_end = static_cast<char*>(addr) + size;
}

// libnx calls this at the end of __appInit: services and the SD are up, the static constructors have not run yet
// (so switch_crash_hooks.c and main.cpp already see the argv[0] chosen here). Under hbloader (NRO) it does nothing.
extern "C" void userAppInit(void) {
  if (!envIsNso()) {
    Diag("[package] userAppInit: NRO (hbloader), packaged mode off\n");
    return;
  }
  const Result mount = romfsMountSelf("romfs");
  Diag("[package] userAppInit: NSO, argc %d, romfsMountSelf rc 0x%X\n", __system_argc, unsigned(mount));
  if (R_FAILED(mount)) return;  // an NSP without RomFS: behave like before
  struct stat st;
  if (stat(kMarker, &st) != 0) {
    Diag("[package] userAppInit: no %s (errno %d), packaged mode off\n", kMarker, errno);
    romfsUnmount("romfs");
    return;
  }
  std::snprintf(g_data_dir, sizeof(g_data_dir), "%s", kDefaultDataDir);
  ReadMarker();
  MakeDirs(g_data_dir);
  // What hbloader does for an NRO (libnx __libnx_init_cwd: chdir to the NRO's folder), which an NSO never gets:
  // newlib's chdir also makes the SD the default device, so rooted paths ("/switch/...", what GetExecutablePath
  // returns) and relative ones resolve exactly as under the NRO. Without it the NSO's view of rooted paths depends on
  // which device happened to become the default.
  const devoptab_t* before = GetDeviceOpTab("");
  const int cd = chdir(g_data_dir);
  const devoptab_t* after = GetDeviceOpTab("");
  Diag("[package] userAppInit: default device before '%s', chdir(%s) %d (errno %d), default device now '%s'\n",
       before && before->name ? before->name : "(none)", g_data_dir, cd, cd ? errno : 0,
       after && after->name ? after->name : "(none)");
  std::snprintf(g_anchor, sizeof(g_anchor), "%s/%s", g_data_dir, kAnchorName);
  int anchor = stat(g_anchor, &st);
  if (anchor != 0) {
    if (std::FILE* f = std::fopen(g_anchor, "w")) {
      std::fputs(kAnchorText, f);
      std::fclose(f);
    }
    anchor = stat(g_anchor, &st);
  }
  // The rooted form is what GetExecutablePath (sdk/src/core/filesystem_posix.cpp) needs to find.
  const int rooted = stat(g_anchor + 5, &st);
  g_active = true;
  // The application loader passes no arguments (argc 0); keep any that some launcher did pass.
  const bool set_argv = __system_argc <= 0 || !__system_argv || !__system_argv[0] || !__system_argv[0][0];
  if (set_argv) {
    __system_argc = 1;
    __system_argv = g_argv;
  }
  Diag("[package] userAppInit: marker found, data_dir %s, anchor stat %d, rooted stat %d (errno %d), argv[0] %s%s\n",
       g_data_dir, anchor, rooted, rooted ? errno : 0, __system_argv[0], set_argv ? " (set here)" : " (kept)");
}

// After switch_crash_hooks.c (101) has pointed stderr at rex_stderr.log.
__attribute__((constructor(150))) static void FlushPackagedDiagnostics() {
  if (g_diag_len) std::fputs(g_diag, stderr);
}

namespace me::packaged {

bool Active() { return g_active; }

const char* DataDir() { return g_active ? g_data_dir : ""; }

std::filesystem::path DataFile(std::string_view name) {
  if (g_active) return std::filesystem::path(kRomfsRoot) / std::string(name);
  return rex::filesystem::GetExecutableFolder() / std::string(name);
}

std::filesystem::path ShippedFile(const std::filesystem::path& name) {
  if (name.empty() || !name.is_relative()) return name;
  const std::filesystem::path sd = rex::filesystem::GetExecutableFolder() / name;
  if (!g_active) return sd;
  std::error_code ec;
  if (std::filesystem::is_regular_file(sd, ec)) return sd;
  return std::filesystem::path(kRomfsRoot) / name;
}

void ConfigurePaths(rex::PathConfig& paths) {
  if (!g_active) return;
  std::error_code ec;
  const std::filesystem::path romfs(kRomfsRoot);
  // Device paths ("romfs:/x") are kept as they are: libstdc++ sees no root directory in them, so nothing may call
  // std::filesystem::absolute on them (sdk/src/system/runtime.cpp leaves them alone on Switch).
  if (paths.game_data_root.empty() && std::filesystem::is_directory(romfs / "game_root", ec)) {
    paths.game_data_root = romfs / "game_root";
  }
  // A masseffect.toml the user put in the SD folder wins (and the settings overlay can save to it); otherwise the
  // packaged one, read-only.
  if (!paths.config_path.empty() && !std::filesystem::exists(paths.config_path, ec)) {
    const auto packaged_config = romfs / paths.config_path.filename();
    if (std::filesystem::exists(packaged_config, ec)) paths.config_path = packaged_config;
  }
  // Same priority as a command-line flag: the packaged toml cannot turn these back.
  // The VFS index exists to avoid a stat per file on the SD; RomFS keeps its tables in memory, and the index file
  // would be keyed on the parent of game_root (romfs:/, read-only) anyway.
  rex::cvar::SetFlagFromCommandLine("vfs_index", "false");
  const bool dlc = std::filesystem::is_directory(romfs / "masseffect" / "0000000000000000", ec);
  if (dlc) {
    rex::cvar::SetFlagFromCommandLine("content_marketplace_root", (romfs / "masseffect").string());
  }
  std::error_code ec_game;
  std::fprintf(stderr, "[package] ConfigurePaths: romfs:/game_root directory %d, config '%s', DLC in RomFS %d\n",
               int(std::filesystem::is_directory(romfs / "game_root", ec_game)), paths.config_path.string().c_str(),
               int(dlc));
}

void ReportPaths(const rex::PathConfig& paths) {
  {
    const devoptab_t* dev = GetDeviceOpTab("");
    char cwd[FS_MAX_PATH] = "";
    if (!getcwd(cwd, sizeof(cwd))) std::snprintf(cwd, sizeof(cwd), "(error %d)", errno);
    // The same SD file once rooted (as the game builds its paths) and once with the device: they must agree.
    char rooted[FS_MAX_PATH], device[FS_MAX_PATH];
    std::snprintf(rooted, sizeof(rooted), "%s/masseffect.toml", g_active ? g_data_dir + 5 : "/switch/masseffect-nx");
    std::snprintf(device, sizeof(device), "sdmc:%s", rooted);
    struct stat a, b;
    const int ra = stat(rooted, &a), rb = stat(device, &b);
    std::fprintf(stderr, "[package] files: default device '%s', cwd '%s', stat %s %d (size %lld), stat %s %d (size %lld)\n",
                 dev && dev->name ? dev->name : "(none)", cwd, rooted, ra, ra ? -1LL : (long long)a.st_size, device, rb,
                 rb ? -1LL : (long long)b.st_size);
  }
  std::error_code ec;
  const bool game_dir = !paths.game_data_root.empty() && std::filesystem::is_directory(paths.game_data_root, ec);
  std::error_code ec_config;
  const bool config = std::filesystem::exists(paths.config_path, ec_config);
  std::fprintf(stderr,
               "[package] paths: packaged %d, executable folder '%s', game_data_root '%s' (directory %d%s%s), "
               "config '%s' (exists %d), user data '%s'\n",
               int(g_active), rex::filesystem::GetExecutableFolder().string().c_str(),
               paths.game_data_root.string().c_str(), int(game_dir), ec ? ", " : "", ec ? ec.message().c_str() : "",
               paths.config_path.string().c_str(), int(config), paths.user_data_root.string().c_str());
}

void LogStatus() {
  if (!g_active) return;
  REXLOG_INFO("[package] installed NSP: read-only data from romfs:/ (game_root, shaders, toml{}), writable folder {}",
              std::filesystem::is_directory("romfs:/masseffect/0000000000000000") ? ", DLC" : "", g_data_dir);
}

}  // namespace me::packaged

#else  // !__SWITCH__: host builds never run packaged.

namespace me::packaged {

bool Active() { return false; }

const char* DataDir() { return ""; }

std::filesystem::path DataFile(std::string_view name) {
  return rex::filesystem::GetExecutableFolder() / std::string(name);
}

std::filesystem::path ShippedFile(const std::filesystem::path& name) {
  if (name.empty() || !name.is_relative()) return name;
  return rex::filesystem::GetExecutableFolder() / name;
}

void ConfigurePaths(rex::PathConfig&) {}

void ReportPaths(const rex::PathConfig&) {}

void LogStatus() {}

}  // namespace me::packaged

#endif
