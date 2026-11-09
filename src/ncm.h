// NetEase Cloud Music lyric + playback feed.
//
// Design constraint: the island shows one synced lyric line, so we need
//   (1) which song is playing  -> NetEase's own window title ("<title> - <artist>")
//   (2) the lyric timeline     -> music.163.com API (search + /api/song/lyric)
//   (3) where we are in it     -> the highest-precision source available:
//       a client memory sample, an optional read-only localhost RPC, or an
//       audio-gated clock.
//
// Position sources, in priority order:
//   a) cloudmusic.dll memory reader (highest precision). ReadProcessMemory at
//      RVA 0x2215AD0 + offset 0xB8, reverse-engineered in netease-lyric.
//      While available it supplies the engine's own millisecond position and
//      play/pause state (position advanced within 0.35 s).
//   b) local RPC  GET <rpc_base>/status, falling back to /info — read-only
//      endpoints of the community "CloudMusic Local API". An explicit base is
//      used when supplied; otherwise http://127.0.0.1:47251 is probed. The
//      service's control/seek endpoints are never called.
//   c) window title + audio-gated clock (fallback): WASAPI peak meters scoped
//      to the cloudmusic.exe sessions on the default render endpoint, so audio
//      from a browser or another player cannot drive the clock. This path
//      cannot observe seek/replay jumps and reports position_exact=false.
//
// Idle gate: a new window title is only accepted as a song change when there is
// playback evidence — a working memory position reader, audibly active
// CloudMusic sessions right now, or a fresh RPC sample that reports the client
// playing. CloudMusic's title also follows browsing/selection while nothing is
// playing, so without evidence the previous identity, lyrics and position are
// preserved instead of switching to whatever the window happens to show. A
// title with no song part never erases identity.
//
// Position: a memory sample wins over everything; an RPC sample can anchor the
// clock when memory is unavailable. Small residual error is eased in, but a
// sample that disagrees with the clock by a seek-sized margin re-anchors
// immediately, and a paused clock never advances on its own.
//
// Seeking: this feed only performs a synthetic click on the player's own
// visible progress strip, and only while that pixel currently belongs to the
// player process (`WindowFromPoint`). A hidden, minimized or covered player is
// not dispatchable: seek_ms() returns false and leaves every field untouched.
// The localhost service's seek endpoint is deliberately never used because it
// restores and foregrounds the window.
//
// Network work happens on a worker thread; update() never blocks the UI frame.
// Lyrics are cached on disk (`<cache_dir>/lyrics-<slug>-<fnv64>.json`, schema 1
// JSON) so a repeated track plays back without network. The key is derived from
// lower(title) + '\n' + lower(artist), because the cache must be resolvable
// before the search call knows the song id.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ncm {

enum class PlayState { Unknown, Playing, Paused };

struct TrackInfo {
  std::string title;
  std::string artist;
  std::string album;
};

struct LyricLine {
  int64_t t_ms = 0;      // start time from the LRC tag
  std::string text;      // original lyric (may be empty for instrumental gaps)
  std::string tr;        // translated line, when the API has tlyric
};

struct LyricDoc {
  int64_t song_id = 0;
  std::string title;
  std::string artist;
  std::vector<LyricLine> lines;  // sorted by t_ms
  std::string cover_bytes;  // encoded artwork for this exact song, fetched off the UI thread
  bool synced = false;           // false => plain/absent lyric, treat as unsynced
  std::string origin;            // "rpc+remote" | "webdb+remote" | "rpc+cache" | "none"
};

struct PlaybackState {
  bool client_running = false;   // a cloudmusic.exe window was found
  TrackInfo track;
  PlayState state = PlayState::Unknown;  // memory clock advancing, else CloudMusic audio activity
  double position_ms = 0.0;      // best known position (frozen while paused)
  double duration_ms = 0.0;      // total song duration in ms
  bool position_exact = false;   // true only while a live memory/RPC sample anchors the clock
  std::string source;            // "memory" | "rpc" | "seek-pending" | "title+clock" | "none"
  double track_changed_at = 0.0; // monotonic seconds, for UI transition animations
  float volume = 1.0f;           // per-process CloudMusic volume (0.0 .. 1.0)
  bool muted = false;            // mute status
  double volume_changed_at = 0.0;// monotonic seconds when volume changed (for HUD animation)
};

struct Config {
  std::string cache_dir = "cache";
  std::string rpc_base;          // explicit RPC base; empty probes http://127.0.0.1:47251
  bool audio_gate = true;        // freeze the fallback clock unless a CloudMusic session is audible
};

class Controller {
 public:
  Controller();
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  void configure(const Config& cfg);

  // Call once per frame with the frame delta; internals are throttled.
  void update(double dt);

  const PlaybackState& state() const;

  // Snapshot of the current track's lyric document (mutex-guarded copy).
  LyricDoc lyrics() const;
  bool lyrics_pending() const;

  // Index of the line that should be visible at `position_ms` (-1 when none).
  int line_at(double position_ms) const;

  // Shift the clock by `delta_ms` (user sync nudge, e.g. +/-500 ms).
  void nudge(double delta_ms);

  // Volume control (WASAPI ISimpleAudioVolume for cloudmusic.exe)
  float volume() const;
  bool is_muted() const;
  bool set_volume(float level);
  bool adjust_volume(float delta);
  bool set_muted(bool mute);
  bool toggle_mute();

  // Media playback controls. Track changes and play/pause go through the
  // system media keys; volume is per-process WASAPI ISimpleAudioVolume.
  bool next_track();
  bool prev_track();
  bool toggle_play_pause();
  // Native-progress click only. Returns false when the player is not running
  // or its visible progress strip does not currently own the target pixel;
  // no position, duration or pending-seek field is changed in that case.
  bool seek_ms(double target_ms);
  // One-line diagnostics for the HUD ("memory · 12 lines · rpc+remote").
  std::string status() const;
 private:
  struct Impl;
  Impl* impl_;
};

// Installed NetEase client found on this machine (window enumeration helper used
// by both the feed and the probe binary). Empty when the client is not running.
std::string netease_window_title();

}  // namespace ncm
