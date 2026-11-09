// ncmctl — console front end for the NetEase Cloud Music data layer.
//
//   ncmctl status [--seconds N] [--rpc BASE] [--cache DIR] [--no-audio-gate]
//   ncmctl title
//   ncmctl next | prev | toggle
//   ncmctl volume <0..1>
//   ncmctl seek <ms>
//
// Every statement is one UTF-8 JSON object on one line, flushed immediately, so
// the tool can be piped or asserted from a shell. `status` samples once per
// second for --seconds seconds (default 1) and then exits.
//
// The feed learns its state only while it is ticking (window title, memory
// position, meter, lyric worker), so every mode that needs state runs it in the
// same update(1.0/60.0) + sleep loop the HUD uses. Exit codes: 0 ok,
// 1 action rejected or nothing found, 2 usage.
#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "ncm.h"

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
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
  return out;
}

const char* play_state_name(ncm::PlayState s) {
  switch (s) {
    case ncm::PlayState::Playing: return "playing";
    case ncm::PlayState::Paused: return "paused";
    default: return "unknown";
  }
}

const char* bool_name(bool b) { return b ? "true" : "false"; }

double elapsed_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void print_line(const char* text) {
  std::fputs(text, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

// Same shape as the vibe-mode headless probe: one line per sample.
void emit_status(const ncm::Controller& feed, double t) {
  const ncm::PlaybackState& st = feed.state();
  const ncm::LyricDoc doc = feed.lyrics();
  const int line = feed.line_at(st.position_ms);
  const bool have_line = line >= 0 && line < static_cast<int>(doc.lines.size());
  const std::string line_text = have_line ? doc.lines[line].text : std::string();
  const std::string line_tr = have_line ? doc.lines[line].tr : std::string();

  char buf[4096];
  std::snprintf(
      buf, sizeof(buf),
      "{\"mode\":\"netease\",\"t\":%.1f,\"client\":%s,\"source\":\"%s\",\"state\":\"%s\",\"title\":\"%s\","
      "\"artist\":\"%s\",\"position_ms\":%.0f,\"duration_ms\":%.0f,\"volume\":%.2f,\"exact\":%s,\"line\":%d,"
      "\"line_text\":\"%s\",\"line_tr\":\"%s\",\"lyric_song_id\":%lld,\"lyric_lines\":%d,\"synced\":%s,"
      "\"lyric_origin\":\"%s\",\"pending\":%s,\"status\":\"%s\"}",
      t, bool_name(st.client_running), json_escape(st.source).c_str(), play_state_name(st.state),
      json_escape(st.track.title).c_str(), json_escape(st.track.artist).c_str(), st.position_ms,
      st.duration_ms, static_cast<double>(st.volume), bool_name(st.position_exact), line,
      json_escape(line_text).c_str(), json_escape(line_tr).c_str(),
      static_cast<long long>(doc.song_id), static_cast<int>(doc.lines.size()), bool_name(doc.synced),
      json_escape(doc.origin).c_str(), bool_name(feed.lyrics_pending()),
      json_escape(feed.status()).c_str());
  print_line(buf);
}

int usage() {
  std::fputs(
      "ncmctl - NetEase Cloud Music controller\n"
      "\n"
      "usage:\n"
      "  ncmctl status [--seconds N] [--rpc BASE] [--cache DIR] [--no-audio-gate]\n"
      "      One JSON object per second for N seconds (default 1).\n"
      "  ncmctl title\n"
      "      Current client window title.\n"
      "  ncmctl next | prev | toggle\n"
      "      One system media key.\n"
      "  ncmctl volume <0..1>\n"
      "      Per-process CloudMusic volume.\n"
      "  ncmctl seek <ms>\n"
      "      Click the player's own progress strip; rejected when its window\n"
      "      does not own that pixel.\n"
      "\n"
      "exit: 0 ok, 1 rejected/not found, 2 usage\n",
      stderr);
  return 2;
}

bool parse_number(const char* text, double* out) {
  if (!text || !*text) return false;
  char* end = nullptr;
  const double value = std::strtod(text, &end);
  if (end == text || (end && *end != '\0')) return false;
  if (!std::isfinite(value)) return false;
  *out = value;
  return true;
}

int run_status(int argc, char** argv) {
  int seconds = 1;
  ncm::Config cfg;
  for (int i = 0; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      seconds = std::atoi(argv[i + 1]);
      ++i;
    } else if (std::strcmp(argv[i], "--rpc") == 0 && i + 1 < argc) {
      cfg.rpc_base = argv[i + 1];
      ++i;
    } else if (std::strcmp(argv[i], "--cache") == 0 && i + 1 < argc) {
      cfg.cache_dir = argv[i + 1];
      ++i;
    } else if (std::strcmp(argv[i], "--no-audio-gate") == 0) {
      cfg.audio_gate = false;
    } else {
      std::fprintf(stderr, "ncmctl: unknown status option: %s\n", argv[i]);
      return usage();
    }
  }
  if (seconds < 0) seconds = 0;

  ncm::Controller feed;
  feed.configure(cfg);

  const auto t0 = std::chrono::steady_clock::now();
  int printed = 0;
  for (;;) {
    feed.update(1.0 / 60.0);
    const double elapsed = elapsed_since(t0);
    // One sample per whole second, at t = 0 .. seconds-1, never more.
    if (printed < seconds && elapsed >= static_cast<double>(printed)) {
      emit_status(feed, elapsed);
      ++printed;
    }
    if (printed >= seconds && elapsed >= static_cast<double>(seconds)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }
  return 0;
}

int run_title() {
  const std::string title = ncm::netease_window_title();
  char buf[2048];
  std::snprintf(buf, sizeof(buf), "{\"mode\":\"title\",\"found\":%s,\"title\":\"%s\"}",
                bool_name(!title.empty()), json_escape(title).c_str());
  print_line(buf);
  return title.empty() ? 1 : 0;
}

int run_media_key(const char* mode) {
  ncm::Controller feed;
  bool ok = false;
  if (std::strcmp(mode, "next") == 0) ok = feed.next_track();
  else if (std::strcmp(mode, "prev") == 0) ok = feed.prev_track();
  else if (std::strcmp(mode, "toggle") == 0) ok = feed.toggle_play_pause();

  char buf[128];
  std::snprintf(buf, sizeof(buf), "{\"mode\":\"%s\",\"ok\":%s}", mode, bool_name(ok));
  print_line(buf);
  return ok ? 0 : 1;
}

int run_volume(const char* text) {
  double level = 0.0;
  if (!parse_number(text, &level)) return usage();
  if (level < 0.0) level = 0.0;
  if (level > 1.0) level = 1.0;

  ncm::Controller feed;
  const bool ok = feed.set_volume(static_cast<float>(level));

  char buf[128];
  std::snprintf(buf, sizeof(buf), "{\"mode\":\"volume\",\"ok\":%s,\"volume\":%.3f,\"muted\":%s}",
                bool_name(ok), static_cast<double>(feed.volume()),
                bool_name(feed.is_muted()));
  print_line(buf);
  return ok ? 0 : 1;
}

int run_seek(const char* text) {
  double target_ms = 0.0;
  if (!parse_number(text, &target_ms)) return usage();

  ncm::Controller feed;
  // The seek decision reads the client window and the track duration, both of
  // which only exist once the feed has ticked; warm it up briefly first.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < deadline) {
    feed.update(1.0 / 60.0);
    if (feed.state().client_running && !feed.state().track.title.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
  }

  const bool ok = feed.seek_ms(target_ms);
  if (ok) {
    // seek_ms delivers its click on a detached thread (~80 ms of cursor moves
    // and input). Exiting now would kill the request before it lands, so keep
    // ticking the feed long enough for the click to complete.
    const auto settle = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < settle) {
      feed.update(1.0 / 60.0);
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  char buf[128];
  std::snprintf(buf, sizeof(buf), "{\"mode\":\"seek\",\"ok\":%s,\"position_ms\":%.0f}",
                bool_name(ok), target_ms);
  print_line(buf);
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  // The window title and lyrics are UTF-8; match the console when attached.
  ::SetConsoleOutputCP(CP_UTF8);

  if (argc < 2) return usage();
  const std::string mode = argv[1];
  if (mode == "--help" || mode == "-h" || mode == "/?") return usage();

  if (mode == "status") return run_status(argc - 2, argv + 2);
  if (mode == "title") return run_title();
  if (mode == "next" || mode == "prev" || mode == "toggle") {
    if (argc > 2) return usage();
    return run_media_key(mode.c_str());
  }
  if (mode == "volume") {
    if (argc != 3) return usage();
    return run_volume(argv[2]);
  }
  if (mode == "seek") {
    if (argc != 3) return usage();
    return run_seek(argv[2]);
  }

  std::fprintf(stderr, "ncmctl: unknown mode: %s\n", argv[1]);
  return usage();
}
