// Mass Effect - developer USB file channel: read logs, status, profiles and caches (and write small files) over the
// USB-C cable while the game runs, instead of sys-ftpd. Off unless dev_usb_files = true. Protocol, VID:PID, host tool
// and limits: docs/usb-files.md; host side: tools/switch_usb.py in mass-effect-recomp.
//
// One libnx thread (never std::thread on Horizon), Horizon priority dev_usb_files_priority (0x3F by default, the
// lowest an application thread can have, so it only runs when nothing else of the game wants the core), ideal core 2
// with mask 0x7 (the ring thread lives on cores 0-1). It owns usbComms entirely: usbCommsRead/Write block (until a
// host reads, or until the cable is plugged back in), and they are only ever called from this thread. With the cvar
// off nothing here runs: no thread, no usb:ds session, no memory.
//
// Files are read with the native FS API (pread semantics, any size). Horizon's FS refuses to open a file a second
// time while it is open for writing; the current log is open for appending by the non-blocking log writer, so GET
// and STAT fall back to RexLogReadCurrentFile (sdk/src/core/log_nonblocking.cpp), which reads it under the writer's
// file lock. Other files held open for writing (logs/rex/rex_stderr.log, the stdio stderr of this process) cannot be
// read while the game runs.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

REXCVAR_DEFINE_BOOL(dev_usb_files, false, "Developer",
                    "USB file channel: a low-priority thread serves LIST/STAT/GET/PUT of files in the NRO folder over "
                    "the USB-C cable (usbComms, 057E:3000, vendor interface FF/4D/45) for tools/switch_usb.py. "
                    "Developer builds only; docs/usb-files.md")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(dev_usb_files_priority, 0x3F, "Developer",
                     "USB file channel: Horizon priority of its thread (0x2C most urgent allowed .. 0x3F lowest)")
    .range(0x2C, 0x3F)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_SWITCH

#include <switch.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

extern "C" int __system_argc;
extern "C" char** __system_argv;
// sdk/src/ui/switch_perf.cpp: thread name in logs/rex/rex_profile.log.
extern "C" void RexSwitchPerfSetThreadName(u32 handle, const char* name);
// sdk/src/core/log_nonblocking.cpp: read the current log while its writer keeps it open.
extern "C" long long RexLogReadCurrentFile(const char* path, unsigned long long offset, void* buf, size_t len,
                                          unsigned long long* size_out);

namespace me::usb_files {
namespace {

// ---- protocol (docs/usb-files.md; keep in sync with tools/switch_usb.py) ---------------------------------------
constexpr u16 kVid = 0x057E;  // libnx usbComms default (Nintendo / "Nintendo Switch")
constexpr u16 kPid = 0x3000;
constexpr u8 kIfClass = 0xFF;  // vendor specific
constexpr u8 kIfSubClass = 0x4D;  // 'M'
constexpr u8 kIfProtocol = 0x45;  // 'E'
constexpr u16 kVersion = 1;
constexpr size_t kReqSize = 1000;  // fixed; not a multiple of any max packet size, so it always ends in a short packet
constexpr size_t kRespSize = 80;   // fixed; same reason (a response header is never merged with what follows)
constexpr size_t kReqBuf = 0x1000;
constexpr size_t kChunk = 1u << 20;
constexpr size_t kMaxRel = 768;  // longest relative path in a request

enum Cmd : u16 { kPing = 1, kList = 2, kStat = 3, kGet = 4, kPut = 5 };
enum Status : s32 { kOk = 0, kBadRequest = 1, kBadPath = 2, kNotFound = 3, kIoError = 4, kLocked = 5, kIsDir = 6 };
constexpr u32 kFlagTail = 1;  // GET: arg0 counts from the end of the file

struct Request {
  u16 cmd = 0;
  u32 seq = 0;
  u32 flags = 0;
  u64 arg0 = 0;
  u64 arg1 = 0;
  std::string path;  // as sent (relative to the NRO folder)
};

struct Response {
  u16 cmd = 0;
  u32 seq = 0;
  s32 status = kOk;
  u64 payload = 0;
  u64 arg0 = 0;
  u64 arg1 = 0;
  u64 arg2 = 0;
  u32 result = 0;
};

// ---- state (allocated by Start, only when the cvar is on) -------------------------------------------------------
Thread g_thread;
std::atomic<bool> g_started{false};
u8* g_buf = nullptr;   // kChunk, page aligned: file data in and out (usbComms then transfers it without a copy)
u8* g_req = nullptr;   // kReqBuf, page aligned
u8* g_resp = nullptr;  // kReqBuf, page aligned
FsDirectoryEntry* g_entries = nullptr;
constexpr size_t kEntries = 32;
FsFileSystem* g_fs = nullptr;
FsFileSystem g_own_fs;
char g_base[FS_MAX_PATH] = "/switch/masseffect-nx";  // NRO folder on the SD card, no device, no trailing '/'
bool g_pending = false;  // g_req holds a request received while a PUT was waiting for data

// Every fsFs* call reads FS_MAX_PATH bytes from the path pointer: paths always live in buffers of that size.
struct FsPath {
  char s[FS_MAX_PATH] = {};
};

void SleepMs(u64 ms) { svcSleepThread(s64(ms) * 1000000LL); }
u64 NowNs() { return armTicksToNs(armGetSystemTick()); }

template <typename T>
T Load(const u8* p) {
  T v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}
template <typename T>
void Store(u8* p, T v) {
  std::memcpy(p, &v, sizeof(v));
}

void InitBase() {
  if (__system_argc < 1 || !__system_argv || !__system_argv[0]) return;
  std::string nro = __system_argv[0];
  const size_t bar = nro.rfind('/');
  if (bar == std::string::npos || bar == 0) return;
  std::string dir = nro.substr(0, bar);
  if (dir.starts_with("sdmc:")) dir.erase(0, 5);
  if (dir.find(':') != std::string::npos || dir.empty() || dir.size() >= FS_MAX_PATH - kMaxRel - 16) return;
  std::snprintf(g_base, sizeof(g_base), "%s", dir.c_str());
}

// Relative path -> SD path of a file in the NRO folder. Rejects "..", backslashes, ':' and control characters;
// drops "." and empty components. "" (or "/") is the NRO folder itself.
bool Resolve(const std::string& rel, FsPath& out, std::string* clean = nullptr) {
  if (rel.size() > kMaxRel) return false;
  std::string norm;
  size_t i = 0;
  while (i <= rel.size()) {
    size_t j = rel.find('/', i);
    if (j == std::string::npos) j = rel.size();
    const std::string_view part(rel.data() + i, j - i);
    i = j + 1;
    if (part.empty() || part == ".") continue;
    if (part == "..") return false;
    for (const char c : part) {
      if (c == '\\' || c == ':' || static_cast<unsigned char>(c) < 0x20) return false;
    }
    norm += '/';
    norm += part;
  }
  const int n = std::snprintf(out.s, sizeof(out.s), "%s%s", g_base, norm.c_str());
  if (n <= 0 || size_t(n) >= sizeof(out.s)) return false;
  if (clean) *clean = norm.empty() ? std::string(".") : norm.substr(1);
  return true;
}

std::string StdioPath(const FsPath& p) { return std::string("sdmc:") + p.s; }

std::string ResultText(Result rc) {
  char text[48];
  std::snprintf(text, sizeof(text), "0x%X (%04u-%04u)", rc, 2000 + R_MODULE(rc), R_DESCRIPTION(rc));
  return text;
}

u64 MTime(const FsPath& p) {
  FsTimeStampRaw ts{};
  if (R_SUCCEEDED(fsFsGetFileTimeStampRaw(g_fs, p.s, &ts)) && ts.is_valid) return ts.modified;
  return 0;
}

// ---- USB I/O ---------------------------------------------------------------------------------------------------
// Both return false on a short or failed transfer (cable pulled, host gone): the caller treats it as a link error.
bool Send(const u8* data, size_t n) {
  if (!n) return true;
  return usbCommsWrite(data, n) == n;
}

bool SendResponse(const Response& r) {
  std::memset(g_resp, 0, kRespSize);
  std::memcpy(g_resp, "MEUR", 4);
  Store<u16>(g_resp + 4, kVersion);
  Store<u16>(g_resp + 6, r.cmd);
  Store<u32>(g_resp + 8, r.seq);
  Store<s32>(g_resp + 12, r.status);
  Store<u64>(g_resp + 16, r.payload);
  Store<u64>(g_resp + 24, r.arg0);
  Store<u64>(g_resp + 32, r.arg1);
  Store<u64>(g_resp + 40, r.arg2);
  Store<u32>(g_resp + 48, r.result);
  return Send(g_resp, kRespSize);
}

// Header + a text payload (LIST output, PING text, error messages), sent through g_buf in chunks.
bool SendWithText(Response r, const std::string& text) {
  r.payload = text.size();
  if (!SendResponse(r)) return false;
  for (size_t off = 0; off < text.size(); off += kChunk) {
    const size_t n = std::min(kChunk, text.size() - off);
    std::memcpy(g_buf, text.data() + off, n);
    if (!Send(g_buf, n)) return false;
  }
  return true;
}

bool SendError(const Request& q, s32 status, const std::string& message, Result rc = 0) {
  Response r;
  r.cmd = q.cmd;
  r.seq = q.seq;
  r.status = status;
  r.result = rc;
  REXLOG_WARN("[usb_files] {} {}: {}", q.cmd, q.path, message);
  return SendWithText(r, message);
}

// ---- commands --------------------------------------------------------------------------------------------------
bool DoPing(const Request& q) {
  Response r;
  r.cmd = q.cmd;
  r.seq = q.seq;
  r.arg0 = kChunk;
  char text[FS_MAX_PATH + 64];
  std::snprintf(text, sizeof(text), "masseffect-nx usb_files v%u base=sdmc:%s", unsigned(kVersion), g_base);
  return SendWithText(r, text);
}

bool DoList(const Request& q) {
  FsPath dir;
  if (!Resolve(q.path, dir)) return SendError(q, kBadPath, "bad path");
  FsDir d;
  Result rc = fsFsOpenDirectory(g_fs, dir.s, FsDirOpenMode_ReadDirs | FsDirOpenMode_ReadFiles, &d);
  if (R_FAILED(rc)) return SendError(q, kNotFound, "cannot open directory: " + ResultText(rc), rc);
  std::string text;
  size_t count = 0;
  for (;;) {
    s64 got = 0;
    rc = fsDirRead(&d, &got, kEntries, g_entries);
    if (R_FAILED(rc) || got <= 0) break;
    for (s64 i = 0; i < got; ++i) {
      const FsDirectoryEntry& e = g_entries[i];
      const bool is_dir = e.type == FsDirEntryType_Dir;
      FsPath child;
      u64 mtime = 0;
      if (std::snprintf(child.s, sizeof(child.s), "%s/%s", dir.s, e.name) < int(sizeof(child.s))) mtime = MTime(child);
      char line[FS_MAX_PATH + 64];
      std::snprintf(line, sizeof(line), "%c\t%lld\t%llu\t%s\n", is_dir ? 'd' : 'f',
                    is_dir ? 0LL : static_cast<long long>(e.file_size), static_cast<unsigned long long>(mtime),
                    e.name);
      text += line;
      ++count;
    }
  }
  fsDirClose(&d);
  Response r;
  r.cmd = q.cmd;
  r.seq = q.seq;
  r.arg0 = count;
  REXLOG_INFO("[usb_files] LIST {} -> {} entries", q.path.empty() ? "." : q.path, count);
  return SendWithText(r, text);
}

// Size of a file that may be open for writing elsewhere: a read-only open, else the current log, else the directory
// entry of its parent (the size FS reports for the open file).
enum class Source { kNone, kFile, kLog };
Source OpenForRead(const FsPath& p, FsFile& f, u64& size, Result& rc) {
  rc = fsFsOpenFile(g_fs, p.s, FsOpenMode_Read, &f);
  if (R_SUCCEEDED(rc)) {
    s64 s = 0;
    if (R_SUCCEEDED(fsFileGetSize(&f, &s))) {
      size = u64(s);
      return Source::kFile;
    }
    fsFileClose(&f);
    return Source::kNone;
  }
  unsigned long long s = 0;
  if (RexLogReadCurrentFile(StdioPath(p).c_str(), 0, nullptr, 0, &s) >= 0) {
    size = s;
    return Source::kLog;
  }
  return Source::kNone;
}

bool DoStat(const Request& q) {
  FsPath p;
  if (!Resolve(q.path, p)) return SendError(q, kBadPath, "bad path");
  FsDirEntryType type;
  Result rc = fsFsGetEntryType(g_fs, p.s, &type);
  if (R_FAILED(rc)) return SendError(q, kNotFound, "not found: " + ResultText(rc), rc);
  Response r;
  r.cmd = q.cmd;
  r.seq = q.seq;
  r.arg1 = MTime(p);
  r.arg2 = type == FsDirEntryType_Dir ? 1 : 0;
  if (type != FsDirEntryType_Dir) {
    FsFile f;
    u64 size = 0;
    const Source src = OpenForRead(p, f, size, rc);
    if (src == Source::kFile) fsFileClose(&f);
    if (src == Source::kNone) {
      r.arg2 = 2;  // a file that cannot be opened now (held open for writing): size from its directory entry
      FsPath parent = p;
      char* bar = std::strrchr(parent.s, '/');
      FsDir d;
      if (bar && bar != parent.s) {
        *bar = 0;
        if (R_SUCCEEDED(fsFsOpenDirectory(g_fs, parent.s, FsDirOpenMode_ReadFiles, &d))) {
          s64 got = 0;
          while (R_SUCCEEDED(fsDirRead(&d, &got, kEntries, g_entries)) && got > 0) {
            for (s64 i = 0; i < got; ++i) {
              if (std::strcmp(g_entries[i].name, bar + 1) == 0) size = u64(g_entries[i].file_size);
            }
          }
          fsDirClose(&d);
        }
      }
    }
    r.arg0 = size;
  }
  return SendResponse(r);
}

bool DoGet(const Request& q) {
  FsPath p;
  if (!Resolve(q.path, p)) return SendError(q, kBadPath, "bad path");
  FsDirEntryType type;
  Result rc = fsFsGetEntryType(g_fs, p.s, &type);
  if (R_FAILED(rc)) return SendError(q, kNotFound, "not found: " + ResultText(rc), rc);
  if (type == FsDirEntryType_Dir) return SendError(q, kIsDir, "is a directory");
  FsFile f;
  u64 size = 0;
  const Source src = OpenForRead(p, f, size, rc);
  if (src == Source::kNone) {
    return SendError(q, kLocked, "cannot open (held open for writing by the game?): " + ResultText(rc), rc);
  }
  u64 start = 0;
  if (q.flags & kFlagTail) {
    start = size > q.arg0 ? size - q.arg0 : 0;
  } else {
    start = std::min(q.arg0, size);
  }
  u64 n = size - start;
  if (q.arg1 && q.arg1 < n) n = q.arg1;
  Response r;
  r.cmd = q.cmd;
  r.seq = q.seq;
  r.payload = n;
  r.arg0 = size;
  r.arg1 = MTime(p);
  r.arg2 = start;
  const u64 t0 = NowNs();
  bool ok = SendResponse(r);
  u64 padded = 0;
  const std::string stdio = src == Source::kLog ? StdioPath(p) : std::string();
  for (u64 off = 0; ok && off < n;) {
    const size_t want = size_t(std::min<u64>(kChunk, n - off));
    size_t got = 0;
    if (src == Source::kFile) {
      u64 read = 0;
      if (R_SUCCEEDED(fsFileRead(&f, s64(start + off), g_buf, want, FsReadOption_None, &read))) got = size_t(read);
    } else {
      const long long read = RexLogReadCurrentFile(stdio.c_str(), start + off, g_buf, want, nullptr);
      if (read > 0) got = size_t(read);
    }
    // The host expects exactly n bytes: a file that shrank (or a read error) is padded with zeros.
    if (got < want) {
      std::memset(g_buf + got, 0, want - got);
      padded += want - got;
    }
    ok = Send(g_buf, want);
    off += want;
  }
  if (src == Source::kFile) fsFileClose(&f);
  const double ms = double(NowNs() - t0) / 1e6;
  REXLOG_INFO("[usb_files] GET {} {}+{} of {}{} -> {} in {:.0f} ms ({:.1f} MB/s){}", q.path, start, n, size,
              src == Source::kLog ? " (current log)" : "", ok ? "sent" : "LINK ERROR", ms,
              ms > 0 ? double(n) / 1048576.0 / (ms / 1000.0) : 0.0,
              padded ? fmt::format(", {} bytes zero-padded (file shrank or read error)", padded) : std::string());
  return ok;
}

bool LooksLikeRequest(const u8* p, size_t n) { return n == kReqSize && std::memcmp(p, "MEUQ", 4) == 0; }

bool DoPut(const Request& q) {
  FsPath p;
  std::string clean;
  if (!Resolve(q.path, p, &clean) || clean == ".") return SendError(q, kBadPath, "bad path");
  FsPath tmp;
  if (std::snprintf(tmp.s, sizeof(tmp.s), "%s.usbtmp", p.s) >= int(sizeof(tmp.s))) {
    return SendError(q, kBadPath, "path too long");
  }
  // Missing parent folders are created (errors ignored: most exist).
  for (char* c = p.s + std::strlen(g_base) + 1; (c = std::strchr(c, '/')) != nullptr; ++c) {
    FsPath dir;
    std::memcpy(dir.s, p.s, size_t(c - p.s));
    fsFsCreateDirectory(g_fs, dir.s);
  }
  fsFsDeleteFile(g_fs, tmp.s);
  Result rc = fsFsCreateFile(g_fs, tmp.s, 0, 0);
  FsFile f;
  if (R_SUCCEEDED(rc)) rc = fsFsOpenFile(g_fs, tmp.s, FsOpenMode_Write | FsOpenMode_Append, &f);
  if (R_FAILED(rc)) return SendError(q, kIoError, "cannot create " + clean + ".usbtmp: " + ResultText(rc), rc);
  Response r;
  r.cmd = q.cmd;
  r.seq = q.seq;
  r.arg0 = q.arg0;
  r.arg2 = 0;  // ready: send the data now
  if (!SendResponse(r)) {
    fsFileClose(&f);
    fsFsDeleteFile(g_fs, tmp.s);
    return false;
  }
  const u64 t0 = NowNs();
  Result write_rc = 0;
  bool link_ok = true;
  u64 off = 0;
  while (off < q.arg0) {
    const size_t want = size_t(std::min<u64>(kChunk, q.arg0 - off));
    const size_t got = usbCommsRead(g_buf, want);
    if (got != want) {
      // A short transfer: the host stopped (or a new host started over). A whole request is kept for the loop.
      if (LooksLikeRequest(g_buf, got)) {
        std::memcpy(g_req, g_buf, got);
        g_pending = true;
      }
      link_ok = got != 0;
      break;
    }
    if (R_SUCCEEDED(write_rc)) write_rc = fsFileWrite(&f, s64(off), g_buf, want, FsWriteOption_None);
    off += want;
  }
  if (R_SUCCEEDED(write_rc)) write_rc = fsFileFlush(&f);
  fsFileClose(&f);
  const double ms = double(NowNs() - t0) / 1e6;
  if (off != q.arg0 || R_FAILED(write_rc)) {
    fsFsDeleteFile(g_fs, tmp.s);
    const std::string why = off != q.arg0 ? fmt::format("transfer stopped after {} of {} bytes", off, q.arg0)
                                          : "write failed: " + ResultText(write_rc);
    REXLOG_WARN("[usb_files] PUT {}: {}", q.path, why);
    if (g_pending || !link_ok) return link_ok;  // nobody waits for this answer
    r.status = kIoError;
    r.result = write_rc;
    r.arg2 = 1;
    return SendWithText(r, why);
  }
  fsFsDeleteFile(g_fs, p.s);
  rc = fsFsRenameFile(g_fs, tmp.s, p.s);
  r.arg2 = 1;  // done
  if (R_FAILED(rc)) {
    fsFsDeleteFile(g_fs, tmp.s);
    r.status = kIoError;
    r.result = rc;
    return SendWithText(r, "rename failed: " + ResultText(rc));
  }
  REXLOG_INFO("[usb_files] PUT {} {} bytes in {:.0f} ms ({:.1f} MB/s)", q.path, q.arg0, ms,
              ms > 0 ? double(q.arg0) / 1048576.0 / (ms / 1000.0) : 0.0);
  return SendResponse(r);
}

// false = link error (the transfer to the host failed).
bool Handle(size_t n, u32& bad_requests) {
  if (n != kReqSize || std::memcmp(g_req, "MEUQ", 4) != 0) {
    // Leftovers of an interrupted transfer: ignored, the host resynchronizes with PING sequence numbers.
    if (bad_requests++ % 64 == 0) REXLOG_WARN("[usb_files] ignored {} bytes that are not a request", n);
    return true;
  }
  Request q;
  const u16 version = Load<u16>(g_req + 4);
  q.cmd = Load<u16>(g_req + 6);
  q.seq = Load<u32>(g_req + 8);
  q.flags = Load<u32>(g_req + 12);
  q.arg0 = Load<u64>(g_req + 16);
  q.arg1 = Load<u64>(g_req + 24);
  const u16 path_len = Load<u16>(g_req + 32);
  if (path_len > kMaxRel || 34 + size_t(path_len) > kReqSize) return SendError(q, kBadRequest, "bad path length");
  q.path.assign(reinterpret_cast<const char*>(g_req + 34), path_len);
  if (version != kVersion) return SendError(q, kBadRequest, fmt::format("protocol version {} (game: {})", version, kVersion));
  switch (q.cmd) {
    case kPing: return DoPing(q);
    case kList: return DoList(q);
    case kStat: return DoStat(q);
    case kGet: return DoGet(q);
    case kPut: return DoPut(q);
    default: return SendError(q, kBadRequest, fmt::format("unknown command {}", q.cmd));
  }
}

void ServerMain(void*) {
  u32 init_failures = 0;
  u32 bad_requests = 0;
  for (;;) {
    const UsbCommsInterfaceInfo info = {kIfClass, kIfSubClass, kIfProtocol};
    const Result rc = usbCommsInitializeEx(1, &info, kVid, kPid);
    if (R_FAILED(rc)) {
      if (init_failures++ % 60 == 0) {
        REXLOG_WARN("[usb_files] usbCommsInitializeEx failed: {} (usb:ds held by another program?); retrying every 5 s",
                    ResultText(rc));
      }
      SleepMs(5000);
      continue;
    }
    REXLOG_INFO("[usb_files] listening: USB {:04X}:{:04X} interface {:02X}/{:02X}/{:02X}, files under sdmc:{}", kVid, kPid,
                kIfClass, kIfSubClass, kIfProtocol, g_base);
    init_failures = 0;
    u32 errors = 0;
    while (errors < 4) {
      size_t n;
      if (g_pending) {
        g_pending = false;
        n = kReqSize;
      } else {
        // Blocks until the host sends something; while the cable is out, inside usbDsWaitReady.
        n = usbCommsRead(g_req, kReqBuf);
      }
      if (n == 0) {
        ++errors;
        SleepMs(500);
        continue;
      }
      if (Handle(n, bad_requests)) {
        errors = 0;
      } else {
        ++errors;
        SleepMs(200);
      }
    }
    REXLOG_WARN("[usb_files] repeated USB errors: restarting usbComms in 2 s");
    usbCommsExit();
    SleepMs(2000);
  }
}

}  // namespace

void Start() {
  if (!REXCVAR_GET(dev_usb_files) || g_started.exchange(true)) return;
  InitBase();
  g_buf = static_cast<u8*>(std::aligned_alloc(0x1000, kChunk));
  g_req = static_cast<u8*>(std::aligned_alloc(0x1000, kReqBuf));
  g_resp = static_cast<u8*>(std::aligned_alloc(0x1000, kReqBuf));
  g_entries = new (std::nothrow) FsDirectoryEntry[kEntries];
  g_fs = fsdevGetDeviceFileSystem("sdmc");
  if (!g_fs && R_SUCCEEDED(fsOpenSdCardFileSystem(&g_own_fs))) g_fs = &g_own_fs;
  if (!g_buf || !g_req || !g_resp || !g_entries || !g_fs) {
    REXLOG_ERROR("[usb_files] not started: out of memory or no SD card filesystem");
    return;
  }
  // The console refuses some low priorities for this process (the prewarm thread only got 0x3B): start at the
  // configured priority and step towards higher ones until threadCreate accepts one (2026-10-09: 0x3F failed).
  int priority = REXCVAR_GET(dev_usb_files_priority);
  Result rc = 0;
  for (; priority >= 0x2C; --priority) {
    rc = threadCreate(&g_thread, ServerMain, nullptr, nullptr, 64 * 1024, priority, 2);
    if (R_SUCCEEDED(rc)) break;
  }
  if (R_FAILED(rc)) {
    REXLOG_ERROR("[usb_files] not started: threadCreate failed (rc 0x{:X})", rc);
    return;
  }
  REXLOG_INFO("[usb_files] thread at priority 0x{:X}", priority);
  RexSwitchPerfSetThreadName(g_thread.handle, "usb files");
  svcSetThreadCoreMask(g_thread.handle, 2, 0x7);
  if (R_FAILED(threadStart(&g_thread))) {
    threadClose(&g_thread);
    REXLOG_ERROR("[usb_files] not started: threadStart failed");
    return;
  }
  REXLOG_INFO("[usb_files] thread started (priority 0x{:X}); see docs/usb-files.md", priority);
}

}  // namespace me::usb_files

#else

namespace me::usb_files {
void Start() {}
}  // namespace me::usb_files

#endif
