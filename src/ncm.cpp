// NetEase Cloud Music feed.
//
// Three inputs, in priority order:
//   1. song identity  — the client's own window title, "<song> - <artist>"
//   2. lyric timeline — music.163.com's public web API (search + /api/song/lyric)
//   3. position       — CloudMusic Local API when the user runs one, otherwise an
//                       audio-gated clock (WASAPI peak meters scoped to the
//                       cloudmusic.exe audio sessions on the default render
//                       endpoint, so other apps cannot drive the clock).
//
// Lyric cache key
// ---------------
// The cache must be resolvable *before* the NetEase song id is known (that is
// what the search call returns), so it is derived from the window title instead:
//
//     <cache_dir>/lyrics-<slug>-<fnv64>.json
//
// <slug>  = title with filesystem-hostile characters folded to '_' (CJK kept,
//           capped at 24 code points) so the file is recognisable by eye.
// <fnv64> = FNV-1a 64 over lower(title) + '\n' + lower(artist); it keeps two
//           same-titled tracks (different artists) in separate files.
// Body    = {"schema":1,"song_id":..,"title":..,"artist":..,"lrc":..,"tlyric":..}
// A second play of the same track therefore never touches the network, which is
// also the offline path.
//
// Threading: update() is UI-thread code and never blocks. All HTTP runs on one
// short-lived worker thread that only ever touches its own refcounted job
// object, so a worker surviving the feed (detach on shutdown) can never reach
// freed state.
//
// Seeking: the only seek this feed performs is a synthetic click on the
// player's own progress strip, and only while that pixel belongs to the
// player's visible, non-minimized window. The localhost service's seek
// endpoint is never called — it restores and foregrounds the window, which the
// feed must never do. A request that cannot be dispatched returns false and
// leaves every field untouched.

#include "ncm.h"

#include <windows.h>
#include <objbase.h>

#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <psapi.h>
#include <sqlite3.h>

#pragma comment(lib, "psapi.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ncm {
namespace {

// ---------------------------------------------------------------- constants

constexpr size_t kMaxHttpBody = 2u * 1024u * 1024u;   // refuse anything bigger
constexpr size_t kMaxCacheBody = 4u * 1024u * 1024u;
constexpr int kHttpConnectTimeoutMs = 3000;
constexpr int kHttpReceiveTimeoutMs = 6000;
constexpr int kHttpResolveTimeoutMs = 3000;
constexpr int kHttpSendTimeoutMs = 3000;

constexpr double kTitlePollSeconds = 0.5;
constexpr double kMeterPollSeconds = 0.1;  // session enumeration is heavier than one endpoint read
constexpr double kPeakThreshold = 1e-4f;
// A real track is rarely silent for long, but quiet passages and moment-to-
// moment meter gaps can dip below the peak threshold. A tight window froze the
// clock through those gaps, so the lyric drifted seconds behind the audio. A
// wide window keeps the clock running through weak sections; a genuine pause
// still stops it after this grace period.
constexpr double kPlayingWindowSeconds = 1.5;
constexpr double kRpcPeriodSeconds = 1.0;  // localhost service; keeps seek latency low
constexpr double kRpcMaxBackoffSeconds = 30.0;
constexpr double kRpcAliveSeconds = 12.0;
constexpr double kRpcEvidenceSeconds = 3.0;  // how long a play flag may authorize identity
constexpr double kRpcCorrectionTau = 0.20;
constexpr double kRpcMaxStepMs = 250.0;
// A sample that disagrees with the clock by more than this is a seek, not
// drift: rewinding is snapped instantly, and so is a forward gap that the
// audio-gated clock cannot explain. While paused the clock cannot drift at
// all, so any larger change is authoritative as well.
constexpr double kSeekBackMs = 400.0;
constexpr double kSeekAheadMs = 1500.0;
constexpr double kPausedSnapMs = 250.0;
// A seek is a request, not a measurement: the client applies it asynchronously
// and may never apply it. The intent holds the requested clock for at most
// kSeekHoldSeconds; a reader ends it early only when a sample sits nearer the
// requested clock than the pre-seek origin, within half the requested jump
// (capped at one click's application latency). Both clocks advance together, so
// a stale pre-seek sample stays exactly one jump away from the requested clock
// and fails for any jump size. Once the hold lapses the readers win again and
// an unapplied seek reverts to measured truth instead of leaving the lyrics on
// a position the client never reached.
constexpr double kSeekConfirmMs = 500.0;
constexpr double kSeekHoldSeconds = 2.0;
constexpr int64_t kTrMergeWindowMs = 40;

const wchar_t* const kUserAgent = L"Mozilla/5.0";
const wchar_t* const kRefererHeader = L"Referer: https://music.163.com\r\n";
const char* const kSearchBase = "https://music.163.com/api/search/get/web?s=";
const char* const kLyricBase = "https://music.163.com/api/song/lyric?id=";
const char* const kSearchTail = "&type=1&limit=10";
const char* const kLyricTail = "&lv=-1&kv=-1&tv=-1";

// ---------------------------------------------------------------- time

double steady_now() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- text

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr,
                                      nullptr);
  if (n <= 0) return std::string();
  std::string out(static_cast<size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::wstring to_wide(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  if (n <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(n), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
  return out;
}

std::string trim(const std::string& s) {
  size_t a = 0;
  size_t b = s.size();
  while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
  while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
  return s.substr(a, b - a);
}

std::string lower_ascii(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

bool ieq_wide(const wchar_t* a, const wchar_t* b) {
  for (;;) {
    wchar_t ca = *a++;
    wchar_t cb = *b++;
    if (ca >= L'A' && ca <= L'Z') ca = static_cast<wchar_t>(ca - L'A' + L'a');
    if (cb >= L'A' && cb <= L'Z') cb = static_cast<wchar_t>(cb - L'B' + L'b');
    if (ca != cb) return false;
    if (ca == L'\0') return true;
  }
}

std::string url_encode(const std::string& s) {
  static const char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                       c == '_' || c == '.' || c == '~';
    if (plain) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

void append_utf8(std::string& out, unsigned cp) {
  if (cp <= 0x7F) {
    out += static_cast<char>(cp);
  } else if (cp <= 0x7FF) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp <= 0xFFFF) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

std::string json_quote(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return out;
}

// ---------------------------------------------------------------- minimal JSON

struct JVal {
  enum Type : uint8_t { Nul, Bool, Num, Str, Arr, Obj };

  Type type = Nul;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;

  bool is_num() const { return type == Num; }
  bool is_str() const { return type == Str; }
  bool is_bool() const { return type == Bool; }
  bool is_arr() const { return type == Arr; }
  bool is_obj() const { return type == Obj; }

  // Case-insensitive member lookup (NetEase keys are lower case; RPC services
  // are not always as disciplined).
  const JVal* get(const char* key) const {
    if (type != Obj) return nullptr;
    const std::string want = lower_ascii(key);
    for (const auto& kv : obj) {
      if (lower_ascii(kv.first) == want) return &kv.second;
    }
    return nullptr;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : p_(text.data()), e_(text.data() + text.size()) {}

  bool parse(JVal& out) {
    skip_ws();
    if (!parse_value(out, 0)) return false;
    skip_ws();
    return p_ == e_;
  }

 private:
  const char* p_;
  const char* e_;

  void skip_ws() {
    while (p_ < e_ && static_cast<unsigned char>(*p_) <= ' ') ++p_;
  }

  bool parse_value(JVal& v, int depth) {
    if (depth > 32) return false;
    skip_ws();
    if (p_ >= e_) return false;
    switch (*p_) {
      case '{': return parse_object(v, depth);
      case '[': return parse_array(v, depth);
      case '"':
        v.type = JVal::Str;
        return parse_string(v.str);
      case 't':
        if (e_ - p_ >= 4 && std::memcmp(p_, "true", 4) == 0) {
          p_ += 4;
          v.type = JVal::Bool;
          v.b = true;
          return true;
        }
        return false;
      case 'f':
        if (e_ - p_ >= 5 && std::memcmp(p_, "false", 5) == 0) {
          p_ += 5;
          v.type = JVal::Bool;
          v.b = false;
          return true;
        }
        return false;
      case 'n':
        if (e_ - p_ >= 4 && std::memcmp(p_, "null", 4) == 0) {
          p_ += 4;
          v.type = JVal::Nul;
          return true;
        }
        return false;
      default: return parse_number(v);
    }
  }

  bool parse_object(JVal& v, int depth) {
    ++p_;  // '{'
    v.type = JVal::Obj;
    skip_ws();
    if (p_ < e_ && *p_ == '}') {
      ++p_;
      return true;
    }
    for (;;) {
      skip_ws();
      if (p_ >= e_ || *p_ != '"') return false;
      std::string key;
      if (!parse_string(key)) return false;
      skip_ws();
      if (p_ >= e_ || *p_ != ':') return false;
      ++p_;
      JVal child;
      if (!parse_value(child, depth + 1)) return false;
      v.obj.emplace_back(std::move(key), std::move(child));
      skip_ws();
      if (p_ < e_ && *p_ == ',') {
        ++p_;
        continue;
      }
      if (p_ < e_ && *p_ == '}') {
        ++p_;
        return true;
      }
      return false;
    }
  }

  bool parse_array(JVal& v, int depth) {
    ++p_;  // '['
    v.type = JVal::Arr;
    skip_ws();
    if (p_ < e_ && *p_ == ']') {
      ++p_;
      return true;
    }
    for (;;) {
      JVal child;
      if (!parse_value(child, depth + 1)) return false;
      v.arr.push_back(std::move(child));
      skip_ws();
      if (p_ < e_ && *p_ == ',') {
        ++p_;
        continue;
      }
      if (p_ < e_ && *p_ == ']') {
        ++p_;
        return true;
      }
      return false;
    }
  }

  bool parse_number(JVal& v) {
    const char* start = p_;
    if (p_ < e_ && (*p_ == '-' || *p_ == '+')) ++p_;
    bool digits = false;
    while (p_ < e_ && *p_ >= '0' && *p_ <= '9') {
      ++p_;
      digits = true;
    }
    if (p_ < e_ && *p_ == '.') {
      ++p_;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') {
        ++p_;
        digits = true;
      }
    }
    if (digits && p_ < e_ && (*p_ == 'e' || *p_ == 'E')) {
      ++p_;
      if (p_ < e_ && (*p_ == '-' || *p_ == '+')) ++p_;
      while (p_ < e_ && *p_ >= '0' && *p_ <= '9') ++p_;
    }
    if (!digits) return false;
    std::string tmp(start, static_cast<size_t>(p_ - start));
    v.type = JVal::Num;
    v.num = std::strtod(tmp.c_str(), nullptr);
    return true;
  }

  bool parse_hex4(unsigned& cp) {
    if (e_ - p_ < 4) return false;
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = *p_++;
      value <<= 4;
      if (c >= '0' && c <= '9') value |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') value |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') value |= static_cast<unsigned>(c - 'A' + 10);
      else return false;
    }
    cp = value;
    return true;
  }

  bool parse_string(std::string& out) {
    if (p_ >= e_ || *p_ != '"') return false;
    ++p_;
    out.clear();
    while (p_ < e_) {
      const unsigned char c = static_cast<unsigned char>(*p_++);
      if (c == '"') return true;
      if (c != '\\') {
        out += static_cast<char>(c);
        continue;
      }
      if (p_ >= e_) return false;
      const char esc = *p_++;
      switch (esc) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp = 0;
          if (!parse_hex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
            const char* save = p_;
            p_ += 2;
            unsigned lo = 0;
            if (parse_hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
              cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
            } else {
              p_ = save;
            }
          }
          if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;  // lone surrogate
          append_utf8(out, cp);
          break;
        }
        default: out += esc; break;
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------- file system

std::string cache_slug(const std::string& title) {
  std::string out;
  size_t i = 0;
  size_t cps = 0;
  while (i < title.size() && cps < 24) {
    const unsigned char c = static_cast<unsigned char>(title[i]);
    size_t len = 1;
    if (c >= 0xF0) len = 4;
    else if (c >= 0xE0) len = 3;
    else if (c >= 0xC0) len = 2;
    if (i + len > title.size()) break;
    if (len > 1) {
      out.append(title, i, len);
      i += len;
      ++cps;
      continue;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_') {
      out += static_cast<char>(c);
    } else if (c == ' ' || c == '.' || c == ',' || c == '\'') {
      out += '_';
    } else if (c < 0x80) {
      out += '_';
    } else {
      break;
    }
    ++i;
    ++cps;
  }
  if (out.empty()) out = "track";
  return out;
}

uint64_t fnv1a64(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ull;
  }
  return h;
}

std::string hex64(uint64_t v) {
  char buf[24];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(v));
  return std::string(buf);
}

std::string cache_path_for(const std::string& dir, const std::string& title, const std::string& artist) {
  const std::string key = lower_ascii(title) + "\n" + lower_ascii(artist);
  const std::string name = "lyrics-" + cache_slug(title) + "-" + hex64(fnv1a64(key)) + ".json";
  std::string base = dir.empty() ? std::string("cache") : dir;
  while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
  if (base.empty()) base = "cache";
  return base + "\\" + name;
}

void ensure_dir(const std::string& dir) {
  if (dir.empty()) return;
  const std::wstring w = to_wide(dir);
  for (size_t i = 1; i < w.size(); ++i) {
    if (w[i] == L'\\' || w[i] == L'/') {
      const std::wstring part = w.substr(0, i);
      ::CreateDirectoryW(part.c_str(), nullptr);
    }
  }
  ::CreateDirectoryW(w.c_str(), nullptr);
}

bool read_file(const std::string& path, std::string& out) {
  out.clear();
  const std::wstring wpath = to_wide(path);
  if (wpath.empty()) return false;
  HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = false;
  if (::GetFileSizeEx(h, &size) && size.QuadPart >= 0 &&
      static_cast<uint64_t>(size.QuadPart) <= kMaxCacheBody) {
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t got = 0;
    ok = true;
    while (got < out.size()) {
      DWORD read = 0;
      const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - got, 1u << 20));
      if (!::ReadFile(h, out.data() + got, want, &read, nullptr) || read == 0) {
        ok = false;
        break;
      }
      got += read;
    }
    out.resize(got);
  }
  ::CloseHandle(h);
  return ok;
}

bool write_file_direct(const std::string& path, const std::string& data) {
  const std::wstring wpath = to_wide(path);
  if (wpath.empty()) return false;
  HANDLE h = ::CreateFileW(wpath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  size_t off = 0;
  while (off < data.size()) {
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>(std::min<size_t>(data.size() - off, 1u << 20));
    if (!::WriteFile(h, data.data() + off, want, &wrote, nullptr) || wrote == 0) {
      ok = false;
      break;
    }
    off += wrote;
  }
  ::CloseHandle(h);
  return ok;
}

bool write_file(const std::string& path, const std::string& data) {
  const std::string tmp = path + ".tmp";
  if (!write_file_direct(tmp, data)) {
    ::DeleteFileW(to_wide(tmp).c_str());
    return false;
  }
  if (!::MoveFileExW(to_wide(tmp).c_str(), to_wide(path).c_str(), MOVEFILE_REPLACE_EXISTING)) {
    ::DeleteFileW(to_wide(tmp).c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- window probe

struct ClientWindow {
  bool found = false;   // a visible cloudmusic.exe window exists
  DWORD pid = 0;
  HWND hwnd = nullptr;
  std::string title;    // its "<song> - <artist>" title, when it has one
};

struct WindowSearch {
  bool found = false;
  DWORD pid = 0;
  HWND hwnd = nullptr;
  std::string title;
};

// A window belongs to the client only when its process image is
// cloudmusic.exe; tab titles and class names alone are app content.
bool is_cloudmusic_process(DWORD pid) {
  if (pid == 0) return false;
  HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!proc) return false;
  wchar_t exe[MAX_PATH];
  DWORD cap = MAX_PATH;
  const BOOL ok = ::QueryFullProcessImageNameW(proc, 0, exe, &cap);
  ::CloseHandle(proc);
  if (!ok) return false;

  const wchar_t* base = exe;
  for (const wchar_t* q = exe; *q; ++q) {
    if (*q == L'\\' || *q == L'/') base = q + 1;
  }
  return ieq_wide(base, L"cloudmusic.exe");
}

BOOL CALLBACK window_enum_proc(HWND hwnd, LPARAM lp) {
  WindowSearch* search = reinterpret_cast<WindowSearch*>(lp);
  if (!::IsWindowVisible(hwnd)) return TRUE;

  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (!is_cloudmusic_process(pid)) return TRUE;

  wchar_t cls[256] = {};
  ::GetClassNameW(hwnd, cls, 256);
  const bool is_orpheus = (_wcsicmp(cls, L"OrpheusBrowserHost") == 0);

  search->found = true;
  if (search->pid == 0 || is_orpheus) {
    search->pid = pid;
    search->hwnd = hwnd;
  }
  if (!search->title.empty() && !is_orpheus) return TRUE;

  const int len = ::GetWindowTextLengthW(hwnd);
  if (len <= 0) return TRUE;
  std::wstring title(static_cast<size_t>(len) + 1, L'\0');
  const int n = ::GetWindowTextW(hwnd, title.data(), len + 1);
  if (n <= 0) return TRUE;
  title.resize(static_cast<size_t>(n));
  if (title.find(L" - ") == std::wstring::npos) return TRUE;

  search->title = to_utf8(title);
  search->pid = pid;
  search->hwnd = hwnd;
  return TRUE;
}

ClientWindow find_client_window() {
  WindowSearch search;
  ::EnumWindows(window_enum_proc, reinterpret_cast<LPARAM>(&search));
  ClientWindow out;
  out.found = search.found;
  out.pid = search.pid;
  out.hwnd = search.hwnd;
  out.title = std::move(search.title);
  return out;
}

// The main player window, only while it is visible. Deliberately independent of
// the title search: a mini player or a dialog must never receive a
// progress-strip click.
struct PlayerWindow {
  HWND hwnd = nullptr;
  DWORD pid = 0;
};

BOOL CALLBACK player_window_proc(HWND hwnd, LPARAM lp) {
  PlayerWindow* out = reinterpret_cast<PlayerWindow*>(lp);
  if (!::IsWindowVisible(hwnd)) return TRUE;

  wchar_t cls[256] = {};
  if (::GetClassNameW(hwnd, cls, 256) <= 0) return TRUE;
  if (_wcsicmp(cls, L"OrpheusBrowserHost") != 0) return TRUE;

  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (!is_cloudmusic_process(pid)) return TRUE;

  out->hwnd = hwnd;
  out->pid = pid;
  return FALSE;  // first visible main player wins
}

PlayerWindow find_player_window() {
  PlayerWindow out;
  ::EnumWindows(player_window_proc, reinterpret_cast<LPARAM>(&out));
  return out;
}

// ---------------------------------------------------------------- seek click

// The only seek this feed may perform is a click the player itself would
// receive: its own visible, non-minimized window currently owns the target
// pixel. Nothing is restored, raised or focused, so a hidden, minimized or
// covered player is simply not dispatchable and the caller reports failure.
struct SeekClickPlan {
  HWND hwnd = nullptr;
  DWORD pid = 0;
  POINT pt = {};
};

bool plan_seek_click(double target_ms, double duration_ms, SeekClickPlan* out) {
  if (!out) return false;
  if (!std::isfinite(target_ms) || !std::isfinite(duration_ms) || duration_ms <= 0.0) return false;

  const PlayerWindow window = find_player_window();
  if (!window.hwnd || window.pid == 0) return false;

  // The window can be hidden or minimized between enumeration and now; clicking
  // then would be a restore/foreground action, which is never allowed.
  if (!::IsWindowVisible(window.hwnd) || ::IsIconic(window.hwnd)) return false;
  WINDOWPLACEMENT wp = {sizeof(wp)};
  if (!::GetWindowPlacement(window.hwnd, &wp)) return false;
  if (wp.showCmd == SW_SHOWMINIMIZED || wp.showCmd == SW_SHOWMINNOACTIVE) return false;

  RECT r = {};
  if (!::GetWindowRect(window.hwnd, &r)) return false;
  const int w = r.right - r.left;
  const int h = r.bottom - r.top;
  if (w <= 1 || h <= 1) return false;

  double frac = target_ms / duration_ms;
  if (!(frac >= 0.0)) frac = 0.0;
  if (frac > 1.0) frac = 1.0;
  POINT pt = {};
  pt.x = r.left + static_cast<int>(frac * static_cast<double>(w - 1));
  // The progress strip sits ~10.2% of the window height above the bottom edge;
  // a fixed pixel offset breaks under display scaling.
  pt.y = r.bottom - static_cast<int>(static_cast<double>(h) * 0.102);
  if (pt.x < r.left || pt.x >= r.right || pt.y < r.top || pt.y >= r.bottom) return false;

  // The pixel must already belong to the player process: an overlay, another
  // app or the feed's own HUD sitting there means the click would land outside.
  HWND hit = ::WindowFromPoint(pt);
  if (!hit) return false;
  DWORD hit_pid = 0;
  ::GetWindowThreadProcessId(hit, &hit_pid);
  if (hit_pid == 0 || hit_pid != window.pid) return false;

  out->hwnd = window.hwnd;
  out->pid = window.pid;
  out->pt = pt;
  return true;
}

// Runs on a detached thread and owns everything it touches: the generation
// guard is a refcounted value, so the feed may die while the click is being
// delivered. A click superseded by a newer seek is dropped, so a rapid second
// request cannot be overwritten by the first one landing late.
void dispatch_seek_click(double target_ms, double duration_ms,
                         std::shared_ptr<std::atomic<uint64_t>> seq_guard, uint64_t seq) {
  if (seq_guard->load(std::memory_order_relaxed) != seq) return;

  SeekClickPlan plan;
  if (!plan_seek_click(target_ms, duration_ms, &plan)) return;

  POINT orig = {};
  if (!::GetCursorPos(&orig)) return;
  ::SetCursorPos(plan.pt.x, plan.pt.y);
  ::Sleep(20);

  // The desktop may change between planning and clicking; a click at an
  // unverified pixel would be stray input, so re-plan at the last instant.
  SeekClickPlan verified;
  if (!plan_seek_click(target_ms, duration_ms, &verified)) {
    ::SetCursorPos(orig.x, orig.y);
    return;
  }
  if (verified.pt.x != plan.pt.x || verified.pt.y != plan.pt.y) {
    ::SetCursorPos(verified.pt.x, verified.pt.y);
  }
  if (seq_guard->load(std::memory_order_relaxed) != seq) {
    ::SetCursorPos(orig.x, orig.y);
    return;
  }
  ::mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
  ::Sleep(20);
  ::mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
  ::Sleep(20);
  ::SetCursorPos(orig.x, orig.y);
}

// ---------------------------------------------------------------- Memory reader
// Directly reads live playback position from cloudmusic.dll internal engine struct.
// Reverse-engineered in netease-lyric:
//   RVA_STATIC_PTR = 0x2215AD0 (cloudmusic.dll)
//   OFFSET_POS_MS  = 0xB8
struct MemoryPositionReader {
  static constexpr uintptr_t kRvaStaticPtr = 0x2215AD0;
  static constexpr ptrdiff_t kOffsetPosMs = 0xB8;

  DWORD pid = 0;
  HANDLE process_handle = nullptr;
  uintptr_t dll_base = 0;

  ~MemoryPositionReader() { close(); }

  void close() {
    if (process_handle) {
      ::CloseHandle(process_handle);
      process_handle = nullptr;
    }
    pid = 0;
    dll_base = 0;
  }

  bool attach(DWORD target_pid) {
    if (target_pid == 0) return false;
    if (pid == target_pid && process_handle && dll_base) return true;
    close();

    HANDLE h = ::OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, target_pid);
    if (!h) return false;

    HMODULE mods[1024];
    DWORD needed = 0;
    if (::EnumProcessModulesEx(h, mods, sizeof(mods), &needed, LIST_MODULES_ALL)) {
      int count = static_cast<int>(needed / sizeof(HMODULE));
      for (int i = 0; i < count; ++i) {
        wchar_t name[256];
        if (::GetModuleBaseNameW(h, mods[i], name, 256)) {
          if (_wcsicmp(name, L"cloudmusic.dll") == 0) {
            pid = target_pid;
            process_handle = h;
            dll_base = reinterpret_cast<uintptr_t>(mods[i]);
            return true;
          }
        }
      }
    }
    ::CloseHandle(h);
    return false;
  }

  bool read_position_ms(int32_t* out_pos_ms) {
    if (!process_handle || !dll_base || !out_pos_ms) return false;
    uint64_t obj = 0;
    SIZE_T bytes_read = 0;
    if (!::ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(dll_base + kRvaStaticPtr),
                             &obj, sizeof(obj), &bytes_read) || bytes_read != sizeof(obj)) {
      return false;
    }
    if (obj < 0x10000) return false;

    int32_t pos = -1;
    if (!::ReadProcessMemory(process_handle, reinterpret_cast<LPCVOID>(obj + kOffsetPosMs),
                             &pos, sizeof(pos), &bytes_read) || bytes_read != sizeof(pos)) {
      return false;
    }
    if (pos >= 0) {
      *out_pos_ms = pos;
      return true;
    }
    return false;
  }
};

void split_title(const std::string& raw, std::string& title, std::string& artist) {
  const size_t sep = raw.rfind(" - ");
  if (sep == std::string::npos) {
    title.clear();
    artist.clear();
    return;
  }
  title = trim(raw.substr(0, sep));
  artist = trim(raw.substr(sep + 3));
}

// ---------------------------------------------------------------- HTTP

struct UrlParts {
  bool ok = false;
  bool secure = false;
  INTERNET_PORT port = 80;
  std::wstring host;
  std::wstring path;
};

UrlParts parse_url(const std::string& url) {
  UrlParts out;
  std::string rest;
  if (url.rfind("https://", 0) == 0) {
    out.secure = true;
    out.port = 443;
    rest = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    out.secure = false;
    out.port = 80;
    rest = url.substr(7);
  } else {
    return out;
  }
  const size_t slash = rest.find('/');
  std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
  const std::string path = (slash == std::string::npos) ? std::string("/") : rest.substr(slash);

  const size_t colon = hostport.rfind(':');
  if (colon != std::string::npos) {
    const int port = std::atoi(hostport.c_str() + colon + 1);
    if (port > 0 && port < 65536) out.port = static_cast<INTERNET_PORT>(port);
    hostport.resize(colon);
  }
  if (hostport.empty()) return out;

  out.host = to_wide(hostport);
  out.path = to_wide(path);
  out.ok = !out.host.empty();
  return out;
}

// Whole-body GET. Never blocks longer than the WinHTTP timeouts; `cancel` is
// polled between steps and during the read loop so a stale job dies fast.
bool http_get(const std::string& url, const wchar_t* referer, std::string& out, const std::atomic<bool>* cancel) {
  out.clear();
  const UrlParts parts = parse_url(url);
  if (!parts.ok) return false;

  HINTERNET session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
  if (!session) {
    session = ::WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                            WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (!session) return false;
  ::WinHttpSetTimeouts(session, kHttpResolveTimeoutMs, kHttpConnectTimeoutMs, kHttpSendTimeoutMs,
                       kHttpReceiveTimeoutMs);

  bool ok = false;
  HINTERNET connection = nullptr;
  HINTERNET request = nullptr;
  std::string body;
  do {
    if (cancel && cancel->load(std::memory_order_relaxed)) break;
    connection = ::WinHttpConnect(session, parts.host.c_str(), parts.port, 0);
    if (!connection) break;
    request = ::WinHttpOpenRequest(connection, L"GET", parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                   WINHTTP_DEFAULT_ACCEPT_TYPES, parts.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!request) break;

    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    ::WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));

    if (referer) {
      ::WinHttpAddRequestHeaders(request, referer, static_cast<DWORD>(-1),
                                 WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    }
    if (!::WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) break;
    if (!::WinHttpReceiveResponse(request, nullptr)) break;

    DWORD status = 0;
    DWORD status_size = sizeof(status);
    if (!::WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                               WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX)) {
      break;
    }
    if (status != 200) break;

    bool read_ok = true;
    for (;;) {
      if (cancel && cancel->load(std::memory_order_relaxed)) {
        read_ok = false;
        break;
      }
      DWORD available = 0;
      if (!::WinHttpQueryDataAvailable(request, &available)) {
        read_ok = false;
        break;
      }
      if (available == 0) break;
      if (body.size() + available > kMaxHttpBody) {
        read_ok = false;
        break;
      }
      char buffer[8192];
      DWORD want = available < sizeof(buffer) ? available : static_cast<DWORD>(sizeof(buffer));
      DWORD read = 0;
      if (!::WinHttpReadData(request, buffer, want, &read) || read == 0) {
        read_ok = false;
        break;
      }
      body.append(buffer, read);
    }
    if (read_ok && !(cancel && cancel->load(std::memory_order_relaxed))) ok = true;
  } while (false);

  if (request) ::WinHttpCloseHandle(request);
  if (connection) ::WinHttpCloseHandle(connection);
  ::WinHttpCloseHandle(session);
  if (ok) out.swap(body);
  return ok;
}

// ---------------------------------------------------------------- LRC

struct RawLyric {
  int64_t t_ms = 0;
  std::string text;
};

bool parse_tag_time(const std::string& tag, int64_t& out) {
  const size_t c1 = tag.find(':');
  if (c1 == std::string::npos || c1 == 0) return false;
  for (size_t i = 0; i < c1; ++i) {
    if (tag[i] < '0' || tag[i] > '9') return false;
  }
  const size_t sec_b = c1 + 1;
  size_t sec_e = sec_b;
  while (sec_e < tag.size() && tag[sec_e] >= '0' && tag[sec_e] <= '9') ++sec_e;
  if (sec_e == sec_b) return false;

  int64_t minutes = 0;
  for (size_t i = 0; i < c1; ++i) minutes = minutes * 10 + (tag[i] - '0');
  int64_t seconds = 0;
  for (size_t i = sec_b; i < sec_e; ++i) seconds = seconds * 10 + (tag[i] - '0');

  int64_t frac_ms = 0;
  if (sec_e < tag.size()) {
    const char sep = tag[sec_e];
    if (sep != '.' && sep != ':') return false;
    const size_t f_b = sec_e + 1;
    size_t f_e = f_b;
    while (f_e < tag.size() && tag[f_e] >= '0' && tag[f_e] <= '9') ++f_e;
    if (f_e == f_b || f_e != tag.size()) return false;
    const size_t digits = f_e - f_b;
    int64_t frac = 0;
    for (size_t i = f_b; i < f_e; ++i) frac = frac * 10 + (tag[i] - '0');
    for (size_t k = digits; k < 3; ++k) frac *= 10;
    for (size_t k = 3; k < digits; ++k) frac /= 10;
    frac_ms = frac;
  }
  out = minutes * 60000 + seconds * 1000 + frac_ms;
  return true;
}

bool parse_offset_tag(const std::string& tag, int64_t& out) {
  static const char kPrefix[] = "offset:";
  if (tag.size() < 7) return false;
  for (size_t i = 0; i < 7; ++i) {
    char c = tag[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != kPrefix[i]) return false;
  }
  const char* p = tag.c_str() + 7;
  while (*p == ' ' || *p == '\t') ++p;
  bool negative = false;
  if (*p == '+') {
    ++p;
  } else if (*p == '-') {
    negative = true;
    ++p;
  }
  if (*p < '0' || *p > '9') return false;
  int64_t value = 0;
  while (*p >= '0' && *p <= '9') {
    value = value * 10 + (*p - '0');
    ++p;
  }
  out = negative ? -value : value;
  return true;
}

// [mm:ss.xxx] / [mm:ss] lines, several timestamps per line allowed. Metadata
// tags ([ar:], [ti:], [by:], [offset:]) never become lines; a numeric offset
// is reported back so the caller can shift the whole document.
void parse_lrc(const std::string& src, std::vector<RawLyric>& out, int64_t& offset_ms) {
  out.clear();
  offset_ms = 0;
  size_t pos = 0;
  while (pos < src.size()) {
    const size_t nl = src.find('\n', pos);
    const size_t end = (nl == std::string::npos) ? src.size() : nl;
    std::string_view line(src.data() + pos, end - pos);
    pos = (nl == std::string::npos) ? src.size() : nl + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty()) continue;

    std::vector<int64_t> stamps;
    size_t i = 0;
    while (i < line.size() && line[i] == '[') {
      const size_t close = line.find(']', i + 1);
      if (close == std::string_view::npos) break;
      const std::string tag(line.substr(i + 1, close - i - 1));
      int64_t t = 0;
      if (parse_tag_time(tag, t)) {
        stamps.push_back(t);
      } else {
        int64_t off = 0;
        if (parse_offset_tag(tag, off)) offset_ms = off;
      }
      i = close + 1;
    }
    const std::string text = trim(std::string(line.substr(i)));
    for (int64_t t : stamps) out.push_back(RawLyric{t, text});
  }
}

// LRC bodies from NetEase often open with the credits ([00:00.000] 作词 : …,
// 作曲 : …, 编曲 : …) that the player shows above the lyric. They carry
// timestamps, so without this filter the island displays them as if they were
// sung. Credits are dropped; every blank separator and every real verse stays.
//
//   ^(作词|作詞|作曲|编曲|編曲|制作人|製作人|监制|監製|混音|母带|母帶|录音|錄音|
//     和声|和聲|吉他|贝斯|貝斯|鼓|弦乐|弦樂|键盘|鋼琴|钢琴|出品|发行|發行|统筹|
//     企劃|企划|词|詞|曲|词曲|OP|SP|演唱|原唱)\s*[:：]
//
// (Latin prefixes case-insensitive) or a bare 作词/作詞/作曲/编曲/編曲, or
// copyright boilerplate. Deliberately conservative: a verse only dies when it
// starts with one of those words *and* a colon follows, so "曲终人散" and the
// blank separators survive.
bool starts_with_ci(const std::string& text, const char* prefix) {
  const size_t len = std::strlen(prefix);
  if (text.size() < len) return false;
  for (size_t i = 0; i < len; ++i) {
    char a = text[i];
    char b = prefix[i];
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

bool contains_any(const std::string& text, std::initializer_list<const char*> needles) {
  for (const char* needle : needles) {
    if (text.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool is_credit_line(const std::string& raw) {
  const std::string text = trim(raw);
  if (text.empty()) return false;  // verse separators are part of the timeline

  static const char* const kCreditPrefixes[] = {
      "作词", "作詞", "作曲", "编曲", "編曲", "制作人", "製作人", "监制", "監製", "混音",
      "母带", "母帶", "录音", "錄音", "和声", "和聲", "吉他", "贝斯", "貝斯", "鼓",
      "弦乐", "弦樂", "键盘", "鋼琴", "钢琴", "出品", "发行", "發行", "统筹", "企劃",
      "企划", "词", "詞", "曲", "词曲", "OP", "SP", "演唱", "原唱"};
  for (const char* prefix : kCreditPrefixes) {
    if (!starts_with_ci(text, prefix)) continue;
    size_t i = std::strlen(prefix);
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t')) ++i;
    if (i < text.size() && text[i] == ':') return true;
    // U+FF1A FULLWIDTH COLON
    if (i + 3 <= text.size() && static_cast<unsigned char>(text[i]) == 0xEF &&
        static_cast<unsigned char>(text[i + 1]) == 0xBC && static_cast<unsigned char>(text[i + 2]) == 0x9A) {
      return true;
    }
  }

  static const char* const kBareCredits[] = {"作词", "作詞", "作曲", "编曲", "編曲"};
  for (const char* bare : kBareCredits) {
    if (text == bare) return true;
  }

  // Boilerplate: explicit copyright notices, plus "版权…管理/所有/方" and
  // "未经许可…不得/禁止/翻唱/转载" style warnings. Requiring the second half
  // keeps sung lines like "未经许可的爱情" or "远方" out of the bin.
  if (contains_any(text, {"歌词版权", "版權聲明", "版权声明", "录音作品", "本歌曲"})) return true;
  const bool owner = contains_any(text, {"版权", "版權"}) && contains_any(text, {"管理", "所有", "方"});
  const bool notice = contains_any(text, {"许可", "許可", "授权", "授權"}) &&
                      contains_any(text, {"翻唱", "转载", "轉載", "复制", "複製", "传播", "傳播", "盗用", "侵權",
                                          "侵权", "追究", "法律", "不得", "禁止"});
  return owner || notice;
}

void build_doc(LyricDoc& doc, const std::string& lrc, const std::string& tlyric) {
  doc.lines.clear();
  doc.synced = false;

  std::vector<RawLyric> main_lines;
  std::vector<RawLyric> tr_lines;
  int64_t offset = 0;
  int64_t tr_offset = 0;
  parse_lrc(lrc, main_lines, offset);
  parse_lrc(tlyric, tr_lines, tr_offset);

  if (offset != 0) {
    for (RawLyric& l : main_lines) {
      l.t_ms += offset;
      if (l.t_ms < 0) l.t_ms = 0;
    }
  }
  if (tr_offset != 0) {
    for (RawLyric& l : tr_lines) {
      l.t_ms += tr_offset;
      if (l.t_ms < 0) l.t_ms = 0;
    }
  }

  const auto by_time = [](const RawLyric& a, const RawLyric& b) { return a.t_ms < b.t_ms; };
  std::stable_sort(main_lines.begin(), main_lines.end(), by_time);
  std::stable_sort(tr_lines.begin(), tr_lines.end(), by_time);

  doc.synced = !main_lines.empty();
  doc.lines.reserve(main_lines.size());
  for (const RawLyric& l : main_lines) {
    if (is_credit_line(l.text)) continue;  // credits are never sung
    LyricLine line;
    line.t_ms = l.t_ms;
    line.text = l.text;
    if (!tr_lines.empty()) {
      const auto it = std::lower_bound(tr_lines.begin(), tr_lines.end(), l.t_ms,
                                       [](const RawLyric& a, int64_t v) { return a.t_ms < v; });
      const RawLyric* best = nullptr;
      int64_t best_delta = kTrMergeWindowMs + 1;
      if (it != tr_lines.end()) {
        const int64_t d = it->t_ms >= l.t_ms ? it->t_ms - l.t_ms : l.t_ms - it->t_ms;
        if (d <= kTrMergeWindowMs) {
          best = &*it;
          best_delta = d;
        }
      }
      if (it != tr_lines.begin()) {
        const auto prev = it - 1;
        const int64_t d = prev->t_ms >= l.t_ms ? prev->t_ms - l.t_ms : l.t_ms - prev->t_ms;
        if (d <= kTrMergeWindowMs && d < best_delta) best = &*prev;
      }
      if (best) line.tr = best->text;
    }
    doc.lines.push_back(std::move(line));
  }
}

// ---------------------------------------------------------------- search / lyric

struct NcmCandidate {
  long long id = 0;
  std::string name;
  std::string artists;
  long long duration_ms = 0;
  int score = -1;
};

bool parse_search(const std::string& body, std::vector<NcmCandidate>& out) {
  out.clear();
  JVal root;
  if (!JsonParser(body).parse(root)) return false;
  const JVal* result = root.get("result");
  if (!result) return false;
  const JVal* songs = result->get("songs");
  if (!songs || !songs->is_arr()) return false;

  for (const JVal& song : songs->arr) {
    if (!song.is_obj()) continue;
    NcmCandidate candidate;
    if (const JVal* id = song.get("id"); id && id->is_num()) candidate.id = static_cast<long long>(id->num);
    if (candidate.id <= 0) continue;
    if (const JVal* name = song.get("name"); name && name->is_str()) candidate.name = name->str;
    if (const JVal* artists = song.get("artists"); artists && artists->is_arr()) {
      for (const JVal& a : artists->arr) {
        if (!a.is_obj()) continue;
        const JVal* n = a.get("name");
        if (!n || !n->is_str()) continue;
        if (!candidate.artists.empty()) candidate.artists += '/';
        candidate.artists += n->str;
      }
    }
    if (const JVal* duration = song.get("duration"); duration && duration->is_num()) {
      candidate.duration_ms = static_cast<long long>(duration->num);
    }
    out.push_back(std::move(candidate));
  }
  return !out.empty();
}

void erase_all(std::string& s, const std::string& needle) {
  if (needle.empty()) return;
  size_t p = 0;
  while ((p = s.find(needle, p)) != std::string::npos) s.erase(p, needle.size());
}

// Lowercase, drop "(...)"/"（...）" (with their content) and "feat.", collapse
// runs of whitespace. Used for both the window title and the search hit name.
std::string norm_key(const std::string& in) {
  std::string stripped;
  stripped.reserve(in.size());
  int depth = 0;
  size_t i = 0;
  while (i < in.size()) {
    const unsigned char c = static_cast<unsigned char>(in[i]);
    if (c == '(') {
      ++depth;
      ++i;
      continue;
    }
    if (c == ')') {
      if (depth > 0) --depth;
      ++i;
      continue;
    }
    if (c == 0xEF && i + 3 <= in.size() && static_cast<unsigned char>(in[i + 1]) == 0xBC) {
      const unsigned char c3 = static_cast<unsigned char>(in[i + 2]);
      if (c3 == 0x88) {  // U+FF08 fullwidth left paren
        ++depth;
        i += 3;
        continue;
      }
      if (c3 == 0x89) {  // U+FF09
        if (depth > 0) --depth;
        i += 3;
        continue;
      }
    }
    if (depth == 0) stripped += static_cast<char>(c);
    ++i;
  }

  std::string lowered = lower_ascii(stripped);
  erase_all(lowered, "feat.");
  erase_all(lowered, "feat ");
  erase_all(lowered, "ft.");

  std::string out;
  out.reserve(lowered.size());
  bool pending_space = false;
  for (char c : lowered) {
    if (c == ' ' || c == '\t') {
      pending_space = true;
      continue;
    }
    if (pending_space && !out.empty()) out += ' ';
    pending_space = false;
    out += c;
  }
  return trim(out);
}

std::vector<std::string> artist_tokens(const std::string& who) {
  std::vector<std::string> out;
  std::string current;
  const auto flush = [&]() {
    const std::string token = norm_key(current);
    if (!token.empty()) out.push_back(token);
    current.clear();
  };
  for (size_t i = 0; i < who.size();) {
    const unsigned char c = static_cast<unsigned char>(who[i]);
    if (c == '/' || c == '&' || c == ',' || c == ';' || c == ' ' || c == '\t') {
      flush();
      ++i;
      continue;
    }
    if (c == 0xE3 && i + 3 <= who.size() && static_cast<unsigned char>(who[i + 1]) == 0x80 &&
        static_cast<unsigned char>(who[i + 2]) == 0x81) {  // U+3001 ideographic comma
      flush();
      i += 3;
      continue;
    }
    current += static_cast<char>(c);
    ++i;
  }
  flush();
  return out;
}

bool artists_overlap(const std::string& a, const std::string& b) {
  const std::vector<std::string> left = artist_tokens(a);
  const std::vector<std::string> right = artist_tokens(b);
  for (const std::string& l : left) {
    for (const std::string& r : right) {
      if (l == r) return true;
    }
  }
  return false;
}

// True only when two identities cannot describe the same recording: different
// titles and no artist in common. Missing artist data on either side is not
// treated as a conflict (there is not enough information to reject). The
// window title and the RPC join multiple artists differently ("/" vs " / "),
// so artist tokens are compared rather than the raw strings.
bool identity_conflict(const std::string& title_a, const std::string& artist_a, const std::string& title_b,
                       const std::string& artist_b) {
  if (title_a.empty() || title_b.empty()) return false;
  if (norm_key(title_a) == norm_key(title_b)) return false;
  if (artist_tokens(artist_a).empty() || artist_tokens(artist_b).empty()) return false;
  return !artists_overlap(artist_a, artist_b);
}

// -1 rejects the hit. Exact (normalised) title dominates; artist token overlap
// only breaks ties between hits that already look like the same song.
int score_candidate(const NcmCandidate& candidate, const std::string& wanted_title,
                    const std::vector<std::string>& wanted_artists) {
  const bool title_match = norm_key(candidate.name) == wanted_title;
  const bool has_duration = candidate.duration_ms > 0;
  if (!title_match && !has_duration) return -1;

  int score = title_match ? 100 : 0;
  if (has_duration) score += 1;

  const std::vector<std::string> got = artist_tokens(candidate.artists);
  int overlap = 0;
  for (const std::string& want : wanted_artists) {
    for (const std::string& g : got) {
      if (want == g) {
        ++overlap;
        break;
      }
    }
  }
  return score + overlap * 10;
}

bool fetch_lyric(long long song_id, std::string& lrc, std::string& tlyric, const std::atomic<bool>& cancel) {
  const std::string url = std::string(kLyricBase) + std::to_string(song_id) + kLyricTail;
  std::string body;
  if (!http_get(url, kRefererHeader, body, &cancel)) return false;
  JVal root;
  if (!JsonParser(body).parse(root)) return false;
  if (const JVal* lrc_obj = root.get("lrc")) {
    if (lrc_obj->is_obj()) {
      if (const JVal* text = lrc_obj->get("lyric")) {
        if (text->is_str()) lrc = text->str;
      }
    }
  }
  if (const JVal* tr_obj = root.get("tlyric")) {
    if (tr_obj->is_obj()) {
      if (const JVal* text = tr_obj->get("lyric")) {
        if (text->is_str()) tlyric = text->str;
      }
    }
  }
  return !lrc.empty();
}

bool parse_cache(const std::string& body, LyricDoc& doc) {
  JVal root;
  if (!JsonParser(body).parse(root)) return false;
  const JVal* id = root.get("song_id");
  if (!id || !id->is_num() || id->num <= 0) return false;
  doc.song_id = static_cast<long long>(id->num);
  std::string lrc;
  std::string tlyric;
  if (const JVal* v = root.get("lrc"); v && v->is_str()) lrc = v->str;
  if (const JVal* v = root.get("tlyric"); v && v->is_str()) tlyric = v->str;
  if (lrc.empty()) return false;
  build_doc(doc, lrc, tlyric);
  return true;
}

std::string make_cache_body(const LyricDoc& doc, const std::string& lrc, const std::string& tlyric) {
  std::string out;
  out.reserve(lrc.size() + tlyric.size() + 256);
  out += "{\"schema\":1,\"song_id\":";
  out += std::to_string(doc.song_id);
  out += ",\"title\":";
  out += json_quote(doc.title);
  out += ",\"artist\":";
  out += json_quote(doc.artist);
  out += ",\"lrc\":";
  out += json_quote(lrc);
  out += ",\"tlyric\":";
  out += json_quote(tlyric);
  out += '}';
  return out;
}

// ---------------------------------------------------------------- RPC /info

struct RpcNum {
  double value = 0.0;
  int depth = 0;
  int prio = 0;
};

struct RpcStr {
  std::string value;
  int depth = 0;
  int prio = 0;
};

bool rpc_artist_value(const JVal& v, std::string& out) {
  out.clear();
  if (v.is_str()) {
    out = v.str;
    return !out.empty();
  }
  if (v.is_arr()) {
    for (const JVal& e : v.arr) {
      std::string part;
      if (e.is_str()) {
        part = e.str;
      } else if (e.is_obj()) {
        const JVal* name = e.get("name");
        if (!name) name = e.get("artist_name");
        if (name && name->is_str()) part = name->str;
      }
      if (part.empty()) continue;
      if (!out.empty()) out += '/';
      out += part;
    }
    return !out.empty();
  }
  if (v.is_obj()) {
    if (const JVal* n = v.get("name"); n && n->is_str()) {
      out = n->str;
      return true;
    }
  }
  return false;
}

// Walks the whole document case-insensitively. Shallowest wins, then the
// per-key priority (progress > position > current > played, etc.).
void collect_rpc(const JVal& node, int depth, std::vector<RpcNum>& positions, std::vector<RpcNum>& durations,
                 std::vector<RpcStr>& titles, std::vector<RpcStr>& artists) {
  if (depth > 12) return;
  if (node.is_obj()) {
    for (const auto& kv : node.obj) {
      const std::string key = lower_ascii(kv.first);
      const JVal& value = kv.second;

      int prio = -1;
      if (key == "progress" || key == "progress_ms" || key == "progressms") prio = 0;
      else if (key == "position" || key == "position_ms" || key == "positionms") prio = 1;
      else if (key == "current") prio = 2;
      else if (key == "played") prio = 3;
      else if (key == "current_sec" || key == "current_time" || key == "currenttime") prio = 4;
      if (prio >= 0 && value.is_num()) positions.push_back(RpcNum{value.num, depth, prio});

      prio = -1;
      if (key == "duration" || key == "duration_ms" || key == "durationms") prio = 0;
      else if (key == "total" || key == "total_sec") prio = 1;
      else if (key == "total_time") prio = 2;
      if (prio >= 0 && value.is_num()) durations.push_back(RpcNum{value.num, depth, prio});

      prio = -1;
      if (key == "song_name") prio = 0;
      else if (key == "title") prio = 1;
      else if (key == "name") prio = 2;
      if (prio >= 0 && value.is_str()) titles.push_back(RpcStr{value.str, depth, prio});

      prio = -1;
      if (key == "artist_name") prio = 0;
      else if (key == "artist") prio = 1;
      else if (key == "artists") prio = 2;
      if (prio >= 0) {
        std::string artist;
        if (rpc_artist_value(value, artist)) artists.push_back(RpcStr{std::move(artist), depth, prio});
      }

      collect_rpc(value, depth + 1, positions, durations, titles, artists);
    }
  } else if (node.is_arr()) {
    for (const JVal& e : node.arr) collect_rpc(e, depth + 1, positions, durations, titles, artists);
  }
}

const RpcStr* pick_str(const std::vector<RpcStr>& list) {
  const RpcStr* best = nullptr;
  for (const RpcStr& c : list) {
    if (c.value.empty()) continue;
    if (!best || c.depth < best->depth || (c.depth == best->depth && c.prio < best->prio)) best = &c;
  }
  return best;
}

struct RpcBool {
  bool value = false;
  int depth = 0;
  int prio = 0;
};

// Collects explicit "is the client playing" booleans. Only keys that state it
// directly are accepted; a service that omits the flag leaves the play state
// unknown, which callers must treat as "no playing evidence", never as paused.
void collect_rpc_play_state(const JVal& node, int depth, std::vector<RpcBool>& out) {
  if (depth > 12) return;
  if (node.is_obj()) {
    for (const auto& kv : node.obj) {
      const std::string key = lower_ascii(kv.first);
      const JVal& value = kv.second;
      if (value.is_bool() && (key == "playing" || key == "is_playing" || key == "isplaying")) {
        out.push_back(RpcBool{value.b, depth, 0});
      }
      collect_rpc_play_state(value, depth + 1, out);
    }
  } else if (node.is_arr()) {
    for (const JVal& e : node.arr) collect_rpc_play_state(e, depth + 1, out);
  }
}

const RpcBool* pick_bool(const std::vector<RpcBool>& list) {
  const RpcBool* best = nullptr;
  for (const RpcBool& c : list) {
    if (!best || c.depth < best->depth || (c.depth == best->depth && c.prio < best->prio)) best = &c;
  }
  return best;
}

// Seconds or milliseconds? Prefer the reading that lands inside [0, duration];
// when both fit, seconds wins (that is what the community APIs emit).
double to_ms(double value, double duration_ms) {
  if (value < 0.0) return -1.0;
  if (duration_ms > 0.0) {
    const double as_seconds = value * 1000.0;
    if (as_seconds <= duration_ms + 2000.0) return as_seconds;
    if (value <= duration_ms + 2000.0) return value;
    return -1.0;
  }
  return value < 10000.0 ? value * 1000.0 : value;
}

double duration_to_ms(double value) {
  if (value <= 0.0) return -1.0;
  return value < 10000.0 ? value * 1000.0 : value;
}

// Placeholder zeroes and mixed units are common in /info bodies, so choose the
// best-ranked duration candidate that actually converts to a positive value
// instead of blindly taking the shallowest one.
double pick_duration_ms(const std::vector<RpcNum>& list) {
  const RpcNum* best = nullptr;
  double best_ms = -1.0;
  for (const RpcNum& c : list) {
    const double ms = duration_to_ms(c.value);
    if (ms <= 0.0) continue;
    if (!best || c.depth < best->depth || (c.depth == best->depth && c.prio < best->prio)) {
      best = &c;
      best_ms = ms;
    }
  }
  return best_ms;
}

// ---------------------------------------------------------------- worker job

// Everything the worker produces lands here, and it is published under
// NcmJob::result_mu, so the UI thread never touches a half-written result.
struct NcmResult {
  LyricDoc doc;
  bool rpc_ok = false;
  bool rpc_has_play_state = false;  // /info exposed an explicit playing flag
  bool rpc_playing = false;
  bool rpc_has_position = false;
  double rpc_position_ms = 0.0;
  std::string rpc_title;
  std::string rpc_artist;
  double duration_ms = 0.0;
};

struct NcmJob {
  enum Kind { Lyrics, Rpc };

  Kind kind = Lyrics;
  std::atomic<bool> cancelled{false};  // polled by the worker between steps
  uint64_t epoch = 0;

  // request (set before the thread starts, read-only afterwards)
  std::string title;
  std::string artist;
  std::string cache_dir;
  std::string rpc_base;

  std::mutex result_mu;
  NcmResult result;
  bool done = false;  // guarded by result_mu
};

void fetch_cover(LyricDoc& doc, const std::atomic<bool>& cancel) {
  if (doc.song_id <= 0 || cancel.load(std::memory_order_relaxed)) return;
  const std::string id = std::to_string(doc.song_id);
  std::string body;
  if (!http_get("https://music.163.com/api/song/detail/?id=" + id + "&ids=%5B" + id + "%5D", kRefererHeader, body, &cancel)) return;
  JVal root;
  if (!JsonParser(body).parse(root)) return;
  const JVal* songs = root.get("songs");
  if (!songs || !songs->is_arr() || songs->arr.empty()) return;
  const JVal& song = songs->arr[0];
  const JVal* returned = song.get("id");
  if (!returned || !returned->is_num() || static_cast<long long>(returned->num) != doc.song_id) return;
  const JVal* album = song.get("album");
  if (!album) album = song.get("al");
  if (!album || !album->is_obj()) return;
  const JVal* pic = album->get("picUrl");
  if (!pic || !pic->is_str()) return;
  std::string url = pic->str;
  if (url.compare(0, 7, "http://") == 0) url.replace(0, 7, "https://");
  const size_t slash = url.find('/', 8);
  if (url.compare(0, 8, "https://") != 0 || slash == std::string::npos) return;
  const std::string host = url.substr(8, slash - 8);
  const std::string suffix = ".music.126.net";
  if (host.size() < suffix.size() || host.compare(host.size()-suffix.size(), suffix.size(), suffix) != 0) return;
  url += (url.find('?') == std::string::npos ? "?param=256y256" : "&param=256y256");
  http_get(url, kRefererHeader, doc.cover_bytes, &cancel);
}
// ---------------------------------------------------------------- SQLite webdb.dat
// Reads local cached track metadata from NetEase CloudMusic's webdb.dat database.
struct LocalTrackMeta {
  int64_t song_id = 0;
  int64_t duration_ms = 0;
  std::string title;
  std::string artists;
  std::string album;
  std::string cover_url;
};

std::wstring get_webdb_path() {
  wchar_t local_app_data[MAX_PATH];
  DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", local_app_data, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return L"";
  std::wstring path = local_app_data;
  path += L"\\NetEase\\CloudMusic\\Library\\webdb.dat";
  DWORD attr = ::GetFileAttributesW(path.c_str());
  if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return L"";
  return path;
}

bool query_webdb(const std::string& title, const std::string& artist, LocalTrackMeta& out) {
  std::wstring db_path = get_webdb_path();
  if (db_path.empty()) return false;

  sqlite3* db = nullptr;
  std::string utf8_path = to_utf8(db_path);
  std::string uri = "file:" + utf8_path + "?mode=ro";
  if (sqlite3_open_v2(uri.c_str(), &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_URI | SQLITE_OPEN_NOMUTEX, nullptr) != SQLITE_OK) {
    if (db) sqlite3_close(db);
    return false;
  }
  sqlite3_busy_timeout(db, 200);

  const char* sql = "SELECT id, jsonStr FROM dbTrack WHERE jsonStr LIKE ? LIMIT 5;";
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_close(db);
    return false;
  }

  std::string pattern = "%" + title + "%";
  sqlite3_bind_text(stmt, 1, pattern.c_str(), static_cast<int>(pattern.size()), SQLITE_STATIC);

  bool found = false;
  std::string wanted_title = norm_key(title);
  std::vector<std::string> wanted_artists = artist_tokens(artist);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    int64_t sid = sqlite3_column_int64(stmt, 0);
    const char* jstr = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    if (!jstr) continue;

    JVal root;
    if (!JsonParser(jstr).parse(root) || !root.is_obj()) continue;

    std::string track_name;
    if (const JVal* name_val = root.get("name")) {
      if (name_val->is_str()) track_name = name_val->str;
    }
    if (norm_key(track_name) != wanted_title && !wanted_title.empty()) continue;

    std::string track_artists;
    if (const JVal* arts = root.get("artists")) {
      if (arts->is_arr()) {
        for (size_t i = 0; i < arts->arr.size(); ++i) {
          if (const JVal* aname = arts->arr[i].get("name")) {
            if (aname->is_str()) {
              if (!track_artists.empty()) track_artists += "/";
              track_artists += aname->str;
            }
          }
        }
      }
    }

    if (!wanted_artists.empty()) {
      std::vector<std::string> got_artists = artist_tokens(track_artists);
      bool match = false;
      for (const auto& w : wanted_artists) {
        for (const auto& g : got_artists) {
          if (w == g) { match = true; break; }
        }
        if (match) break;
      }
      if (!match) continue;
    }

    out.song_id = sid;
    out.title = track_name.empty() ? title : track_name;
    out.artists = track_artists.empty() ? artist : track_artists;
    if (const JVal* dur = root.get("duration")) {
      if (dur->is_num()) out.duration_ms = static_cast<int64_t>(dur->num);
    }
    if (const JVal* alb = root.get("album")) {
      if (alb->is_obj()) {
        if (const JVal* aname = alb->get("name")) {
          if (aname->is_str()) out.album = aname->str;
        }
        if (const JVal* pic = alb->get("picUrl")) {
          if (pic->is_str()) out.cover_url = pic->str;
        }
        if (out.cover_url.empty()) {
          if (const JVal* bpic = alb->get("blurPicUrl")) {
            if (bpic->is_str()) out.cover_url = bpic->str;
          }
        }
      }
    }
    found = true;
    break;
  }

  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return found;
}

void run_lyrics_job(NcmJob& job, NcmResult& out) {
  out.doc.origin = "none";
  out.doc.title = job.title;
  out.doc.artist = job.artist;

  const std::string path = cache_path_for(job.cache_dir, job.title, job.artist);
  std::string cached_body;
  if (read_file(path, cached_body)) {
    LyricDoc cached;
    if (parse_cache(cached_body, cached) && !cached.lines.empty()) {
      cached.title = job.title;
      cached.artist = job.artist;
      cached.origin = "rpc+cache";
      fetch_cover(cached, job.cancelled);
      out.doc = std::move(cached);
      LocalTrackMeta local_meta;
      if (query_webdb(job.title, job.artist, local_meta) && local_meta.duration_ms > 0) {
        out.duration_ms = static_cast<double>(local_meta.duration_ms);
      } else if (!out.doc.lines.empty()) {
        out.duration_ms = static_cast<double>(out.doc.lines.back().t_ms + 4000);
      }
      return;
    }
  }
  if (job.cancelled.load(std::memory_order_relaxed)) return;

  // 1. Check local SQLite webdb.dat (instant metadata & song_id from netease-lyric)
  LocalTrackMeta local_meta;
  if (query_webdb(job.title, job.artist, local_meta) && local_meta.song_id > 0) {
    out.duration_ms = static_cast<double>(local_meta.duration_ms);
    std::string lrc, tlyric;
    if (fetch_lyric(local_meta.song_id, lrc, tlyric, job.cancelled)) {
      LyricDoc doc;
      doc.song_id = local_meta.song_id;
      doc.title = job.title;
      doc.artist = job.artist;
      doc.origin = "webdb+remote";
      build_doc(doc, lrc, tlyric);
      if (doc.synced && !doc.lines.empty()) {
        ensure_dir(job.cache_dir);
        write_file(path, make_cache_body(doc, lrc, tlyric));
        fetch_cover(doc, job.cancelled);
        out.doc = std::move(doc);
        return;
      }
    }
  }
  if (job.cancelled.load(std::memory_order_relaxed)) return;

  const std::string search_url = std::string(kSearchBase) + url_encode(job.title) + kSearchTail;
  std::string search_body;
  if (!http_get(search_url, kRefererHeader, search_body, &job.cancelled)) return;

  std::vector<NcmCandidate> candidates;
  if (!parse_search(search_body, candidates)) return;

  const std::string wanted_title = norm_key(job.title);
  const std::vector<std::string> wanted_artists = artist_tokens(job.artist);
  std::vector<size_t> order;
  order.reserve(candidates.size());
  for (size_t i = 0; i < candidates.size(); ++i) {
    candidates[i].score = score_candidate(candidates[i], wanted_title, wanted_artists);
    if (candidates[i].score >= 0) order.push_back(i);
  }
  if (order.empty()) return;
  std::stable_sort(order.begin(), order.end(),
                   [&](size_t a, size_t b) { return candidates[a].score > candidates[b].score; });

  LyricDoc fallback;
  bool have_fallback = false;

  // Best match first; a hit with no usable lyric at all is not worth caching,
  // so the next exact-title hit gets a turn. (Credits are filtered out of the
  // timeline before this, so a one-line "纯音乐，请欣赏" still counts as an
  // answer — trying lower-ranked covers instead would show the wrong words.)
  for (size_t k = 0; k < order.size() && k < 3; ++k) {
    if (job.cancelled.load(std::memory_order_relaxed)) return;
    const NcmCandidate& candidate = candidates[order[k]];
    std::string lrc;
    std::string tlyric;
    if (!fetch_lyric(candidate.id, lrc, tlyric, job.cancelled)) continue;

    LyricDoc doc;
    doc.song_id = candidate.id;
    doc.title = job.title;
    doc.artist = job.artist;
    doc.origin = "rpc+remote";
    build_doc(doc, lrc, tlyric);

    if (doc.synced && !doc.lines.empty()) {
      ensure_dir(job.cache_dir);
      write_file(path, make_cache_body(doc, lrc, tlyric));
      fetch_cover(doc, job.cancelled);
      out.doc = std::move(doc);
      return;
    }
    if (!have_fallback) {
      fallback = std::move(doc);
      have_fallback = true;
    }
  }
  if (!have_fallback && !order.empty()) {
    fallback.song_id = candidates[order[0]].id;
    fallback.title = job.title; fallback.artist = job.artist;
    have_fallback = true;
  }
  if (have_fallback) { fetch_cover(fallback, job.cancelled); out.doc = std::move(fallback); }
}

void run_rpc_job(NcmJob& job, NcmResult& out) {
  std::string base = job.rpc_base;
  while (!base.empty() && (base.back() == '/' || base.back() == ' ')) base.pop_back();
  if (base.empty()) return;
  std::string body;
  bool got = http_get(base + "/status", nullptr, body, &job.cancelled);
  if (!got) {
    got = http_get(base + "/info", nullptr, body, &job.cancelled);
  }
  if (!got) return;

  JVal root;
  if (!JsonParser(body).parse(root)) return;

  std::vector<RpcNum> positions;
  std::vector<RpcNum> durations;
  std::vector<RpcStr> titles;
  std::vector<RpcStr> artists;
  std::vector<RpcBool> play_states;
  collect_rpc(root, 0, positions, durations, titles, artists);
  collect_rpc_play_state(root, 0, play_states);

  out.rpc_ok = true;

  if (const RpcBool* p = pick_bool(play_states)) {
    out.rpc_has_play_state = true;
    out.rpc_playing = p->value;
  }

  const double duration_ms = pick_duration_ms(durations);
  if (duration_ms > 0.0) out.duration_ms = duration_ms;

  if (const RpcStr* t = pick_str(titles)) out.rpc_title = t->value;
  if (const RpcStr* a = pick_str(artists)) out.rpc_artist = a->value;

  // "the field whose value fits 0..duration"
  for (const RpcNum& c : positions) {
    const double ms = to_ms(c.value, duration_ms);
    if (ms < 0.0) continue;
    if (duration_ms > 0.0 && ms > duration_ms + 2000.0) continue;
    out.rpc_position_ms = ms;
    out.rpc_has_position = true;
    break;
  }
  if (!out.rpc_has_position) {
    for (const RpcNum& c : positions) {
      const double ms = to_ms(c.value, -1.0);
      if (ms >= 0.0) {
        out.rpc_position_ms = ms;
        out.rpc_has_position = true;
        break;
      }
    }
  }
}

void run_job(std::shared_ptr<NcmJob> job) {
  NcmResult result;
  if (job->kind == NcmJob::Lyrics) run_lyrics_job(*job, result);
  else run_rpc_job(*job, result);
  {
    std::lock_guard<std::mutex> lock(job->result_mu);
    job->result = std::move(result);
    job->done = true;
  }
}

}  // namespace

// ---------------------------------------------------------------- Impl

struct Controller::Impl {
  Config cfg;
  PlaybackState state;

  mutable std::mutex mu;  // guards `doc` (published by update, read by lyrics())
  LyricDoc doc;

  std::atomic<bool> pending{false};

  // memory reader (highest precision direct playback position from cloudmusic.dll)
  MemoryPositionReader mem_reader;
  DWORD client_pid = 0;
  int32_t last_mem_pos = -1;
  double last_mem_change_at = 0.0;
  bool mem_available = false;

  // Seek intent: the user-requested jump the client has not confirmed yet.
  // `origin` is the last measured clock before the jump (a real engine sample,
  // never the request itself); while the intent holds, position_ms is a preview
  // and is not exact.
  bool seek_pending = false;
  double seek_target_ms = 0.0;
  double seek_origin_ms = 0.0;
  double seek_issued_at = 0.0;
  double seek_hold_until = 0.0;
  // Native-click generation. A detached click holds a reference, so it can
  // check for a newer request without ever touching this Impl.
  std::shared_ptr<std::atomic<uint64_t>> seek_click_seq =
      std::make_shared<std::atomic<uint64_t>>(0);

  // audio meter (CloudMusic sessions only; a browser playing sound must not
  // count as playback)
  bool com_ready = false;
  bool com_owned = false;
  IMMDeviceEnumerator* enumerator = nullptr;
  IAudioSessionManager2* session_manager = nullptr;
  std::vector<DWORD> cloudmusic_pids;
  double next_meter_at = 0.0;
  double meter_retry_at = 0.0;
  double next_pid_refresh_at = 0.0;
  double last_loud_at = -1.0e9;

  // polling bookkeeping
  double next_title_at = 0.0;
  double next_rpc_at = 0.0;
  double last_update_at = 0.0;
  int rpc_failures = 0;

  // exact-position follow
  double rpc_target_ms = 0.0;
  double rpc_target_at = 0.0;
  double rpc_alive_until = 0.0;

  // last successful RPC sample, used as playback/identity evidence
  double rpc_sample_at = -1.0e9;
  bool rpc_has_play_state = false;
  bool rpc_playing = false;
  bool rpc_has_identity = false;
  std::string rpc_title;
  std::string rpc_artist;

  // track identity / lyric fetching
  uint64_t epoch = 0;
  bool need_lyrics = false;
  std::thread worker;
  std::shared_ptr<NcmJob> job;

  ~Impl() {
    if (session_manager) {
      session_manager->Release();
      session_manager = nullptr;
    }
    if (enumerator) {
      enumerator->Release();
      enumerator = nullptr;
    }
  }

  void publish(LyricDoc next) {
    std::lock_guard<std::mutex> lock(mu);
    doc = std::move(next);
  }

  // Sampled straight from the meter rather than from `state.state`, which is
  // only refreshed later in the frame (title polling runs before tick()).
  bool audio_playing(double now) const {
    return com_ready && session_manager && now - last_loud_at <= kPlayingWindowSeconds;
  }

  bool clock_advancing() const { return cfg.audio_gate ? state.state == PlayState::Playing : true; }

  bool seek_in_flight(double now) const { return seek_pending && now < seek_hold_until; }

  void clear_seek_pending() {
    seek_pending = false;
    seek_hold_until = 0.0;
  }

  // The local clock while the jump is unconfirmed: the requested target, running
  // forward only while playback is actually advancing.
  double seek_clock_ms(double now, bool advancing) const {
    const double elapsed = advancing ? (now - seek_issued_at) * 1000.0 : 0.0;
    return seek_target_ms + (elapsed > 0.0 ? elapsed : 0.0);
  }

  // A sample confirms the jump when it is nearer the requested clock than the
  // pre-seek origin, and that residual fits inside half the jump (capped at one
  // click's application latency). Seeking backwards is not special-cased:
  // proximity to the requested clock already excludes every pre-seek sample,
  // including a jump shorter than a single poll interval.
  bool seek_confirmed(double sample_ms, double now, bool advancing) const {
    const double jump = seek_target_ms - seek_origin_ms;
    const double tolerance = std::min(kSeekConfirmMs, std::fabs(jump) * 0.5);
    const double target_clock = seek_clock_ms(now, advancing);
    const double to_target = std::fabs(sample_ms - target_clock);
    const double to_origin = std::fabs(sample_ms - seek_origin_ms);
    return to_target < to_origin && to_target <= tolerance;
  }

  bool rpc_playing_evidence(double now) const {
    // Shorter than the position freshness window: a dead or wedged RPC must
    // not keep authorizing track switches for long.
    return now - rpc_sample_at <= kRpcEvidenceSeconds && rpc_has_play_state && rpc_playing;
  }

  bool rpc_identity_conflict(const std::string& title, const std::string& artist) const {
    if (!rpc_has_identity) return false;
    return identity_conflict(title, artist, rpc_title, rpc_artist);
  }

  // CloudMusic's title follows browsing and selection, not only playback. Adopt
  // a new title only when the CloudMusic-scoped audio meter says sound is
  // flowing right now or the RPC reports the client playing. Otherwise the
  // previous identity, lyrics and position stay; nothing is guessed from a
  // title alone.
  bool title_change_trusted(double now) const {
    if (!cfg.audio_gate) return true;
    if (mem_available) return true;
    if (audio_playing(now)) return true;
    return rpc_playing_evidence(now);
  }

  void set_track(const std::string& title, const std::string& artist, double now) {
    state.track.title = title;
    state.track.artist = artist;
    state.track.album.clear();
    state.position_ms = 0.0;
    state.duration_ms = 0.0;
    state.position_exact = false;
    state.track_changed_at = now;
    state.source = title.empty() ? "none" : "title+clock";
    ++epoch;
    need_lyrics = !title.empty();
    rpc_alive_until = 0.0;
    clear_seek_pending();

    LyricDoc fresh;
    fresh.origin = "none";
    publish(std::move(fresh));
  }

  void apply_client(const ClientWindow& window, double now) {
    if (!window.found) {
      if (state.client_running || !state.track.title.empty() || !state.track.artist.empty()) {
        state.client_running = false;
        state.state = PlayState::Unknown;
        set_track(std::string(), std::string(), now);
      }
      client_pid = 0;
      mem_available = false;
      last_mem_pos = -1;
      mem_reader.close();
      state.source = "none";
      state.position_ms = 0.0;
      state.position_exact = false;
      rpc_alive_until = 0.0;
      rpc_sample_at = -1.0e9;
      rpc_has_play_state = false;
      rpc_playing = false;
      rpc_has_identity = false;
      rpc_title.clear();
      rpc_artist.clear();
      clear_seek_pending();
      return;
    }
    state.client_running = true;
    client_pid = window.pid;
    std::string title;
    std::string artist;
    split_title(window.title, title, artist);
    // A window without a song title (non-player page, chrome only) must not
    // erase a preserved identity, and a title seen while nothing is audibly
    // playing must not replace it either.
    if (title.empty()) return;
    if (title == state.track.title && artist == state.track.artist) return;
    if (!title_change_trusted(now)) return;
    set_track(title, artist, now);
  }

  bool poll_memory(double now) {
    if (!state.client_running || client_pid == 0) {
      mem_available = false;
      return false;
    }
    if (!mem_reader.attach(client_pid)) {
      mem_available = false;
      return false;
    }
    int32_t pos_ms = -1;
    if (!mem_reader.read_position_ms(&pos_ms) || pos_ms < 0) {
      mem_available = false;
      return false;
    }

    mem_available = true;
    const double new_pos = static_cast<double>(pos_ms);
    if (pos_ms != last_mem_pos) {
      last_mem_pos = pos_ms;
      last_mem_change_at = now;
    }

    // Play/Pause detection: with an exact memory clock the advancing position is
    // ground truth. Audio-peak grace is only a fallback when the clock is absent.
    state.state = (now - last_mem_change_at <= 0.35) ? PlayState::Playing : PlayState::Paused;

    // The engine clock is authority, except for a jump the user just asked for
    // that the client has not applied yet. The reader keeps reporting the
    // pre-seek position for a moment; adopting it would snap the HUD (and the
    // lyric) back on the next frame. Hold the requested clock instead, but
    // never call that a measurement: only a corroborating sample may leave the
    // position exact. After the hold the sample wins again.
    const bool advancing = state.state == PlayState::Playing;
    if (seek_in_flight(now) && !seek_confirmed(new_pos, now, advancing)) {
      state.position_ms = seek_clock_ms(now, advancing);
      state.position_exact = false;
      state.source = "seek-pending";
      return true;
    }
    clear_seek_pending();

    state.position_ms = new_pos;
    state.position_exact = true;
    state.source = "memory";
    return true;
  }

  void poll_title(double now) {
    if (now < next_title_at) return;
    next_title_at = now + kTitlePollSeconds;
    apply_client(find_client_window(), now);
  }

  void open_meter() {
    if (!com_ready) return;
    if (!enumerator) {
      IMMDeviceEnumerator* created = nullptr;
      const HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                            __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&created));
      if (FAILED(hr) || !created) return;
      enumerator = created;
    }
    if (session_manager) return;
    IMMDevice* device = nullptr;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) return;
    IAudioSessionManager2* activated = nullptr;
    const HRESULT hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(&activated));
    device->Release();
    if (FAILED(hr) || !activated) return;
    session_manager = activated;
  }

  // CloudMusic renders audio from its main process or a CEF child; both share
  // the image name, so the process list identifies every session that belongs
  // to it without opening each process.
  void refresh_cloudmusic_pids(double now) {
    if (now < next_pid_refresh_at) return;
    next_pid_refresh_at = now + 2.0;
    cloudmusic_pids.clear();
    HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry = {};
    entry.dwSize = static_cast<DWORD>(sizeof(entry));
    if (::Process32FirstW(snapshot, &entry)) {
      do {
        if (ieq_wide(entry.szExeFile, L"cloudmusic.exe")) cloudmusic_pids.push_back(entry.th32ProcessID);
      } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
  }

  bool is_cloudmusic_pid(DWORD pid) const {
    for (DWORD candidate : cloudmusic_pids) {
      if (candidate == pid) return true;
    }
    return false;
  }

  // Peak of the active CloudMusic sessions only. The endpoint-wide meter is
  // deliberately not used: sound from a browser or another player must never
  // count as NetEase playback.
  float cloudmusic_peak() const {
    if (!session_manager) return 0.0f;
    IAudioSessionEnumerator* sessions = nullptr;
    if (FAILED(session_manager->GetSessionEnumerator(&sessions)) || !sessions) return 0.0f;
    int count = 0;
    float peak = 0.0f;
    if (SUCCEEDED(sessions->GetCount(&count))) {
      for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || !control) {
          if (control) control->Release();
          continue;
        }
        AudioSessionState session_state = AudioSessionStateInactive;
        if (SUCCEEDED(control->GetState(&session_state)) && session_state == AudioSessionStateActive) {
          DWORD pid = 0;
          bool belongs = false;
          IAudioSessionControl2* control2 = nullptr;
          if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                reinterpret_cast<void**>(&control2))) &&
              control2) {
            if (SUCCEEDED(control2->GetProcessId(&pid))) belongs = is_cloudmusic_pid(pid);
            control2->Release();
          }
          if (belongs) {
            IAudioMeterInformation* session_meter = nullptr;
            if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioMeterInformation),
                                                  reinterpret_cast<void**>(&session_meter))) &&
                session_meter) {
              float value = 0.0f;
              if (SUCCEEDED(session_meter->GetPeakValue(&value)) && value > peak) peak = value;
              session_meter->Release();
            }
          }
        }
        control->Release();
      }
    }
    sessions->Release();
    return peak;
  }

  float get_cloudmusic_volume(bool* out_muted = nullptr) {
    if (!session_manager) { open_meter(); refresh_cloudmusic_pids(steady_now()); }
    if (!session_manager) return 1.0f;
    IAudioSessionEnumerator* sessions = nullptr;
    if (FAILED(session_manager->GetSessionEnumerator(&sessions)) || !sessions) return 1.0f;
    int count = 0;
    float vol = -1.0f;
    if (SUCCEEDED(sessions->GetCount(&count))) {
      for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || !control) continue;
        DWORD pid = 0;
        bool belongs = false;
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2) {
          if (SUCCEEDED(control2->GetProcessId(&pid))) belongs = is_cloudmusic_pid(pid);
          control2->Release();
        }
        if (belongs) {
          ISimpleAudioVolume* s_vol = nullptr;
          if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&s_vol))) && s_vol) {
            float v = 0.0f;
            if (SUCCEEDED(s_vol->GetMasterVolume(&v))) vol = v;
            BOOL m = FALSE;
            if (out_muted && SUCCEEDED(s_vol->GetMute(&m))) *out_muted = (m != FALSE);
            s_vol->Release();
          }
        }
        control->Release();
        if (vol >= 0.0f) break;
      }
    }
    sessions->Release();
    return vol >= 0.0f ? vol : 1.0f;
  }

  bool set_cloudmusic_volume(float level, bool update_state = true) {
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    if (!session_manager) { open_meter(); refresh_cloudmusic_pids(steady_now()); }
    if (!session_manager) return false;
    IAudioSessionEnumerator* sessions = nullptr;
    if (FAILED(session_manager->GetSessionEnumerator(&sessions)) || !sessions) return false;
    int count = 0;
    bool ok = false;
    if (SUCCEEDED(sessions->GetCount(&count))) {
      for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || !control) continue;
        DWORD pid = 0;
        bool belongs = false;
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2) {
          if (SUCCEEDED(control2->GetProcessId(&pid))) belongs = is_cloudmusic_pid(pid);
          control2->Release();
        }
        if (belongs) {
          ISimpleAudioVolume* s_vol = nullptr;
          if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&s_vol))) && s_vol) {
            if (SUCCEEDED(s_vol->SetMasterVolume(level, nullptr))) ok = true;
            s_vol->Release();
          }
        }
        control->Release();
      }
    }
    sessions->Release();
    if (ok && update_state) {
      state.volume = level;
      state.volume_changed_at = steady_now();
    }
    return ok;
  }

  bool set_cloudmusic_mute(bool mute) {
    if (!session_manager) { open_meter(); refresh_cloudmusic_pids(steady_now()); }
    if (!session_manager) return false;
    IAudioSessionEnumerator* sessions = nullptr;
    if (FAILED(session_manager->GetSessionEnumerator(&sessions)) || !sessions) return false;
    int count = 0;
    bool ok = false;
    if (SUCCEEDED(sessions->GetCount(&count))) {
      for (int i = 0; i < count; ++i) {
        IAudioSessionControl* control = nullptr;
        if (FAILED(sessions->GetSession(i, &control)) || !control) continue;
        DWORD pid = 0;
        bool belongs = false;
        IAudioSessionControl2* control2 = nullptr;
        if (SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))) && control2) {
          if (SUCCEEDED(control2->GetProcessId(&pid))) belongs = is_cloudmusic_pid(pid);
          control2->Release();
        }
        if (belongs) {
          ISimpleAudioVolume* s_vol = nullptr;
          if (SUCCEEDED(control->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&s_vol))) && s_vol) {
            if (SUCCEEDED(s_vol->SetMute(mute ? TRUE : FALSE, nullptr))) ok = true;
            s_vol->Release();
          }
        }
        control->Release();
      }
    }
    sessions->Release();
    if (ok) {
      state.muted = mute;
      state.volume_changed_at = steady_now();
    }
    return ok;
  }

  static void send_media_key(BYTE vk) {
    ::keybd_event(vk, 0, 0, 0);
    ::Sleep(20);
    ::keybd_event(vk, 0, KEYEVENTF_KEYUP, 0);
  }

  void poll_meter(double now) {
    if (now < next_meter_at) return;
    next_meter_at = now + kMeterPollSeconds;
    if (!com_ready) return;
    if (!session_manager) {
      if (now < meter_retry_at) return;
      meter_retry_at = now + 5.0;
      open_meter();
      if (!session_manager) return;
    }
    refresh_cloudmusic_pids(now);
    const float peak = cloudmusic_peak();
    if (static_cast<double>(peak) > kPeakThreshold) last_loud_at = now;

    bool is_muted = false;
    const float vol = get_cloudmusic_volume(&is_muted);
    if (vol >= 0.0f) {
      if (std::fabs(state.volume - vol) > 0.005f || state.muted != is_muted) {
        state.volume = vol;
        state.muted = is_muted;
        state.volume_changed_at = now;
      }
    }
  }

  bool job_done() const {
    if (!job) return true;
    std::lock_guard<std::mutex> lock(job->result_mu);
    return job->done;
  }

  void pump(double now) {
    if (!job) return;
    if (!job_done()) {
      if (!state.client_running || (job->kind == NcmJob::Lyrics && job->epoch != epoch)) {
        job->cancelled.store(true, std::memory_order_relaxed);
      }
      return;
    }

    NcmResult result;
    {
      std::lock_guard<std::mutex> lock(job->result_mu);
      result = std::move(job->result);
    }
    const NcmJob::Kind kind = job->kind;
    const uint64_t job_epoch = job->epoch;

    if (worker.joinable()) worker.join();
    job.reset();

    if (result.duration_ms > 0.0) state.duration_ms = result.duration_ms;

    if (kind == NcmJob::Lyrics) {
      if (job_epoch == epoch) publish(std::move(result.doc));
    } else {
      apply_rpc(result, now, job_epoch);
    }
  }

  void apply_rpc(const NcmResult& finished, double now, uint64_t job_epoch) {
    if (!finished.rpc_ok) {
      ++rpc_failures;
      int shift = std::min(rpc_failures - 1, 3);
      double wait = kRpcPeriodSeconds * static_cast<double>(1 << shift);
      if (wait > kRpcMaxBackoffSeconds) wait = kRpcMaxBackoffSeconds;
      next_rpc_at = now + wait;
      return;
    }
    rpc_failures = 0;
    next_rpc_at = now + kRpcPeriodSeconds;

    // The request was issued for an older identity: the track changed while it
    // was in flight, so its title/position describe something the HUD has
    // already moved past. Drop it whole and let the next poll (1 s) catch up.
    if (job_epoch != epoch) return;
    if (finished.duration_ms > 0.0) state.duration_ms = finished.duration_ms;

    // Remember the sample even when the client window is momentarily gone: it
    // is playback evidence for the next title poll either way.
    rpc_sample_at = now;
    rpc_has_play_state = finished.rpc_has_play_state;
    rpc_playing = finished.rpc_playing;
    rpc_has_identity = !finished.rpc_title.empty();
    rpc_title = finished.rpc_title;
    rpc_artist = finished.rpc_artist;

    if (!state.client_running) return;

    // The window title is the primary identity; the RPC fills the gap when the
    // client shows no song title (e.g. a non-player page is focused). Only a
    // playing session may introduce a new song: a paused /info still names a
    // loaded track, and that must not make the HUD show a song nobody started.
    if (state.track.title.empty() && rpc_has_identity &&
        (finished.rpc_playing || audio_playing(now))) {
      set_track(finished.rpc_title, finished.rpc_artist, now);
    }

    if (!finished.rpc_has_position) return;
    // A sample that still describes the previous track must not move the new
    // track's clock (identity switches and RPC polls are not atomic).
    if (rpc_identity_conflict(state.track.title, state.track.artist)) return;

    // A seek the client has not applied yet is still the newest intent: this
    // sample describes the pre-seek position, so it must not snap the clock
    // back nor move the follow target. Accept it only when it corroborates the
    // target; once the hold has lapsed the client evidently did not apply the
    // jump and the sample is authority again.
    const bool advancing = clock_advancing();
    if (seek_in_flight(now) && !seek_confirmed(finished.rpc_position_ms, now, advancing)) return;
    clear_seek_pending();

    const bool had_target = state.position_exact;
    double predicted = state.position_ms;
    if (had_target) {
      predicted = rpc_target_ms + (advancing ? (now - rpc_target_at) * 1000.0 : 0.0);
    }
    const double delta = finished.rpc_position_ms - predicted;

    // Drift is small and one-directional; anything seek-sized is authoritative.
    // While paused the clock cannot drift at all, so a larger jump is a seek
    // too. Snapping keeps the old position from driving lyrics after a seek.
    const bool seek = !had_target || delta <= -kSeekBackMs || delta >= kSeekAheadMs ||
                      (!advancing && std::fabs(delta) >= kPausedSnapMs);
    if (seek) state.position_ms = finished.rpc_position_ms;

    rpc_target_ms = finished.rpc_position_ms;
    rpc_target_at = now;
    rpc_alive_until = now + kRpcAliveSeconds;
    state.position_exact = true;
    state.source = "rpc";
  }

  void start_jobs(double now) {
    if (job) return;
    if (need_lyrics && state.client_running && !state.track.title.empty()) {
      std::shared_ptr<NcmJob> fresh = std::make_shared<NcmJob>();
      fresh->kind = NcmJob::Lyrics;
      fresh->epoch = epoch;
      fresh->title = state.track.title;
      fresh->artist = state.track.artist;
      fresh->cache_dir = cfg.cache_dir;
      need_lyrics = false;
      job = fresh;
      worker = std::thread(run_job, fresh);
      return;
    }
    std::string rpc = cfg.rpc_base;
    if (rpc.empty()) {
      rpc = "http://127.0.0.1:47251";
    }
    if (!rpc.empty() && state.client_running && now >= next_rpc_at) {
      std::shared_ptr<NcmJob> fresh = std::make_shared<NcmJob>();
      fresh->kind = NcmJob::Rpc;
      fresh->epoch = epoch;  // a sample from before the next track switch is stale
      fresh->rpc_base = rpc;
      job = fresh;
      next_rpc_at = now + kRpcPeriodSeconds;
      worker = std::thread(run_job, fresh);
    }
  }

  void tick(double dt, double now) {
    if (dt < 0.0) dt = 0.0;

    if (!state.client_running) {
      state.state = PlayState::Unknown;
      return;
    }

    // 1. Native memory reading from cloudmusic.dll (netease-lyric engine)
    if (poll_memory(now)) {
      return;
    }
    if (!com_ready || !session_manager) {
      state.state = PlayState::Unknown;
    } else {
      const PlayState was = state.state;
      state.state = (now - last_loud_at <= kPlayingWindowSeconds) ? PlayState::Playing : PlayState::Paused;
      // The grace window keeps the clock running through quiet passages, but a
      // real pause must not leave that grace added to the position: rewind the
      // span we advanced while already silent. A quiet passage never reaches
      // this branch because sound resumes before the window expires.
      if (was == PlayState::Playing && state.state == PlayState::Paused && !state.position_exact) {
        const double silent = now - last_loud_at;
        const double undo = silent < kPlayingWindowSeconds ? silent : kPlayingWindowSeconds;
        state.position_ms -= undo * 1000.0;
      }
    }

    if (state.position_exact && now > rpc_alive_until) {
      state.position_exact = false;
      state.source = state.track.title.empty() ? "none" : "title+clock";
    }

    const bool advancing = clock_advancing();
    if (advancing) state.position_ms += dt * 1000.0;

    // While a seek is unconfirmed the position is an unmeasured preview
    // (position_exact is false), so easing must not touch it; the confirming
    // sample sets the position and the follow target again.
    if (state.position_exact && !seek_in_flight(now)) {
      const double elapsed = advancing ? (now - rpc_target_at) * 1000.0 : 0.0;
      const double target = rpc_target_ms + (elapsed > 0.0 ? elapsed : 0.0);
      const double error = target - state.position_ms;
      double step = error * (1.0 - std::exp(-dt / kRpcCorrectionTau));
      if (step > kRpcMaxStepMs) step = kRpcMaxStepMs;
      else if (step < -kRpcMaxStepMs) step = -kRpcMaxStepMs;
      state.position_ms += step;
    }

    // No reader corroborated the jump before the hold lapsed: drop the intent
    // so the HUD stops claiming a pending request. Provenance returns to the
    // clock; the position itself is the best remaining estimate and stays
    // labelled inexact.
    if (seek_pending && !seek_in_flight(now)) {
      clear_seek_pending();
      state.position_exact = false;
      state.source = state.track.title.empty() ? "none" : "title+clock";
    }

    if (state.position_ms < 0.0) state.position_ms = 0.0;
  }

  void shutdown_worker() {
    if (job) job->cancelled.store(true, std::memory_order_relaxed);
    if (!worker.joinable()) return;
    // Bounded wait: a job normally finishes in well under a second. If the
    // network is wedged past that, detach — the job owns everything it touches,
    // so nothing dangling is left behind (and no std::terminate).
    for (int i = 0; i < 120; ++i) {
      if (job_done()) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!job_done()) worker.detach();
    else worker.join();
  }
};

// ---------------------------------------------------------------- public API

Controller::Controller() : impl_(new Impl()) {
  impl_->state.source = "none";
  impl_->doc.origin = "none";

  // The meter needs COM on the calling (UI) thread. STA is the GUI convention;
  // if the host already picked an apartment we simply join it.
  const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  if (SUCCEEDED(hr)) {
    impl_->com_ready = true;
    impl_->com_owned = true;
  } else if (hr == RPC_E_CHANGED_MODE) {
    impl_->com_ready = true;
  }
}

Controller::~Controller() {
  // Release the audio-session interfaces while COM is still initialized (the
  // Impl destructor does that), then uninitialize.
  const bool com_owned = impl_->com_owned;
  impl_->shutdown_worker();
  delete impl_;
  if (com_owned) ::CoUninitialize();
}

void Controller::configure(const Config& cfg) {
  impl_->cfg = cfg;
  impl_->next_title_at = 0.0;
  impl_->next_rpc_at = 0.0;
  impl_->rpc_failures = 0;
}

void Controller::update(double dt) {
  Impl& s = *impl_;
  const double now = steady_now();

  // `dt` is the caller's nominal frame delta. A blocked or throttled host loop
  // can be much slower than it claims (the headless probe passes 1/60 while
  // sleeping 16 ms, which Windows rounds to two 15.6 ms ticks), so the playback
  // clock follows the measured wall delta whenever that is sane; `dt` remains
  // the first-frame fallback and the hard ceiling.
  double frame_dt = dt;
  if (s.last_update_at > 0.0) {
    const double measured = now - s.last_update_at;
    if (measured > 0.0) frame_dt = measured < 1.0 ? measured : 1.0;
  }
  s.last_update_at = now;

  s.pump(now);
  // Meter first: the title gate below judges the freshest audio evidence, not
  // the previous frame's play state.
  s.poll_meter(now);
  s.poll_title(now);
  s.tick(frame_dt, now);
  s.start_jobs(now);

  const bool pending =
      s.need_lyrics || (s.job && s.job->kind == NcmJob::Lyrics && !s.job_done());
  s.pending.store(pending, std::memory_order_relaxed);
}

const PlaybackState& Controller::state() const { return impl_->state; }

LyricDoc Controller::lyrics() const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  return impl_->doc;
}

bool Controller::lyrics_pending() const { return impl_->pending.load(std::memory_order_relaxed); }

int Controller::line_at(double position_ms) const {
  std::lock_guard<std::mutex> lock(impl_->mu);
  const std::vector<LyricLine>& lines = impl_->doc.lines;
  if (lines.empty()) return -1;

  int64_t target = 0;
  if (position_ms > 9.0e15) target = INT64_MAX;
  else if (position_ms < -9.0e15) target = INT64_MIN;
  else target = static_cast<int64_t>(std::llround(position_ms));

  size_t lo = 0;
  size_t hi = lines.size();
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (lines[mid].t_ms <= target) lo = mid + 1;
    else hi = mid;
  }
  if (lo == 0) return -1;
  return static_cast<int>(lo - 1);
}

void Controller::nudge(double delta_ms) {
  impl_->state.position_ms += delta_ms;
  if (impl_->state.position_ms < 0.0) impl_->state.position_ms = 0.0;
}

float Controller::volume() const { return impl_->state.volume; }
bool Controller::is_muted() const { return impl_->state.muted; }

bool Controller::set_volume(float level) {
  return impl_->set_cloudmusic_volume(level);
}

bool Controller::adjust_volume(float delta) {
  const float cur = impl_->state.volume;
  float target = cur + delta;
  if (target < 0.0f) target = 0.0f;
  if (target > 1.0f) target = 1.0f;
  return impl_->set_cloudmusic_volume(target);
}

bool Controller::set_muted(bool mute) {
  return impl_->set_cloudmusic_mute(mute);
}

bool Controller::toggle_mute() {
  return impl_->set_cloudmusic_mute(!impl_->state.muted);
}

bool Controller::next_track() {
  impl_->send_media_key(VK_MEDIA_NEXT_TRACK);
  return true;
}

bool Controller::prev_track() {
  impl_->send_media_key(VK_MEDIA_PREV_TRACK);
  return true;
}

bool Controller::toggle_play_pause() {
  impl_->send_media_key(VK_MEDIA_PLAY_PAUSE);
  return true;
}

bool Controller::seek_ms(double target_ms) {
  if (!std::isfinite(target_ms)) return false;
  if (!impl_->state.client_running) return false;
  if (target_ms < 0.0) target_ms = 0.0;
  // Duration is resolved locally: a request that turns out to be undispatchable
  // must leave every field untouched, including this metadata cache.
  double dur = impl_->state.duration_ms;
  if (dur <= 0.0 && !impl_->state.track.title.empty()) {
    LocalTrackMeta meta;
    if (query_webdb(impl_->state.track.title, impl_->state.track.artist, meta) && meta.duration_ms > 0) {
      dur = static_cast<double>(meta.duration_ms);
    }
  }
  if (dur > 0.0 && target_ms > dur) target_ms = dur;

  // 1. Only a jump the player itself can receive may change any state. When its
  //    own window does not currently own the target pixel, nothing is touched:
  //    no preview, no pending intent, and no HTTP control call (the localhost
  //    service's seek endpoint restores and clicks the window, which is exactly
  //    the foreground disturbance this feed must never cause).
  const double issued_at = steady_now();
  SeekClickPlan plan;
  if (!plan_seek_click(target_ms, dur, &plan)) return false;
  if (impl_->state.duration_ms <= 0.0 && dur > 0.0) impl_->state.duration_ms = dur;

  // 2. Record the newest intent. The origin is the last real engine sample, so
  //    a rapid repeat compares against where playback actually is rather than
  //    against the previous request's preview. Without a fresh memory sample,
  //    a still-pending seek keeps its measured origin. The request is never
  //    written into the memory sample fields; those keep describing the engine,
  //    including its play/pause timing.
  double origin = impl_->state.position_ms;
  if (impl_->mem_available && impl_->last_mem_pos >= 0) {
    origin = static_cast<double>(impl_->last_mem_pos);
  } else if (impl_->seek_in_flight(issued_at)) {
    origin = impl_->seek_origin_ms;
  }
  impl_->seek_origin_ms = origin;
  impl_->seek_target_ms = target_ms;
  impl_->seek_issued_at = issued_at;
  impl_->seek_hold_until = issued_at + kSeekHoldSeconds;
  impl_->seek_pending = true;

  // The requested spot is a preview until a reader corroborates it: it is not
  // exact, and the follow target is re-anchored to it so the pre-seek easing
  // target cannot pull the clock back.
  impl_->state.position_ms = target_ms;
  impl_->state.position_exact = false;
  impl_->state.source = "seek-pending";
  impl_->rpc_target_ms = target_ms;
  impl_->rpc_target_at = issued_at;

  // 3. Exactly one native click, re-validated immediately before it lands.
  const uint64_t seq = impl_->seek_click_seq->fetch_add(1, std::memory_order_relaxed) + 1;
  std::thread(dispatch_seek_click, target_ms, dur, impl_->seek_click_seq, seq).detach();
  return true;
}

std::string Controller::status() const {
  Impl& s = *impl_;
  std::string source = s.state.source.empty() ? std::string("none") : s.state.source;
  size_t lines = 0;
  std::string origin;
  {
    std::lock_guard<std::mutex> lock(s.mu);
    lines = s.doc.lines.size();
    origin = s.doc.origin.empty() ? std::string("none") : s.doc.origin;
  }
  std::string out;
  out.reserve(64);
  out += source;
  out += " \xC2\xB7 ";  // middle dot, matching the HUD's typography
  out += std::to_string(lines);
  out += " lines";
  out += " \xC2\xB7 ";
  out += origin;
  return out;
}

std::string netease_window_title() { return find_client_window().title; }

}  // namespace ncm
