# NeteaseCloudMusic-Controller

Windows 专用的网易云音乐（NetEase Cloud Music）系统集成层：识别当前曲目、取同步歌词、跟踪精确播放位置，并控制音量、静音、切歌与进度。

代码从 `vibe-mode` 桌面 HUD 的数据层（`src/data/netease.h` / `netease.cpp`）原样抽取，逻辑未改，仅将命名空间 `vibe` 收敛为 `ncm`、`NeteaseFeed` 改为 `Controller`、`NeteaseConfig` 改为 `Config`，并补上独立构建与 CLI。不依赖 ImGui，不含任何 UI、主题或颜色渲染代码。目标客户端：**NetEase Cloud Music 3.1.46 x64（Build 205627，patch 5d77cd4）**，其他构建为 best-effort。

## 能力

| 能力 | 机制 | 回退 |
| --- | --- | --- |
| 曲目识别 | 枚举网易云主窗口，解析窗口标题 `<歌曲> - <歌手>` | 带 idle gate：仅当存在播放证据（内存位置可读、`cloudmusic.exe` 会话正在发声、或 RPC 采样报告 playing）才接受新标题；否则保留上一首的曲目/歌词/位置不变 |
| 歌词 | `music.163.com` 搜索 + `/api/song/lyric`（含 `lrc` 与 `tlyric` 翻译） | 落盘缓存 `<cache>/lyrics-<slug>-<fnv64>.json`（`schema:1`），同曲二次播放不联网；无同步歌词时按 unsynced 处理 |
| 播放位置 | 内存读取：`ReadProcessMemory`，`cloudmusic.dll` RVA `0x2215AD0` + 偏移 `0xB8`，直接取引擎毫秒位置与播放状态 | ① 只读 RPC：`GET /status`，失败回退 `/info`，默认 `http://127.0.0.1:47251`；② WASAPI 音频门控时钟（峰值表按 `cloudmusic.exe` 音频会话过滤，其他播放器无法驱动时钟） |
| 音量 / 静音 | 进程级 `ISimpleAudioVolume`，只作用于 `cloudmusic.exe` 的音频会话 | 无（无会话即拒绝写入） |
| 播放控制 | 系统媒体键（上一首 / 下一首 / 播放-暂停） | 客户端未运行时返回失败 |
| 跳转进度 | 对播放器自身可见进度条做一次原生点击，且只在 `WindowFromPoint` 该像素属于播放器进程时派发 | 隐藏 / 最小化 / 被遮挡时不派发：`seek_ms()` 返回 `false` 且不改动任何字段（不抢前台、不还原窗口） |
| 元数据 / 封面 | 只读打开 `%LOCALAPPDATA%\NetEase\CloudMusic\Library\webdb.dat`（SQLite）取歌曲 id、时长、封面 | 走在线接口；封面字节在后台线程获取 |

RPC 只使用只读端点（`/status`、`/info`）；本库从不调用任何控制类或 seek 端点。

## 构建

```bat
build.cmd
```

产出 `build\ncmctl.exe`；`build.cmd clean` 删除 `build\`。工具链：MSVC 2022（`vcvars64.bat`）+ Windows SDK `10.0.26100`，x64，`/std:c++20 /O2 /MT`。SQLite 直接编译仓库内自带的 amalgamation（`third_party/sqlite/sqlite3.c`），无需外部下载。

> 构建脚本显式追加 SDK 的 include/lib 路径（不依赖注册表 `KitsRoot10`）；SDK 装在其他位置时，改脚本顶部的 `SDKROOT` / `SDKVER` 即可。

## CLI

```
ncmctl status [--seconds N] [--rpc BASE] [--cache DIR] [--no-audio-gate]
ncmctl title
ncmctl next | prev | toggle
ncmctl volume <0..1>
ncmctl seek <ms>
ncmctl --help
```

- `status`：以 1 Hz 连续输出 JSON Lines，持续 `N` 秒（默认 `1`）后退出。
- `--rpc BASE`：指定只读 RPC 基址（等价 `Config::rpc_base`；缺省探测 `http://127.0.0.1:47251`）。
- `--cache DIR`：歌词缓存目录（等价 `Config::cache_dir`，默认 `cache`）。
- `--no-audio-gate`：关闭音频门控，无发声会话时回退时钟也照常推进（等价 `audio_gate=false`）。
- `title`：只做一次窗口枚举，输出当前标题。
- `next` / `prev` / `toggle`：发一次系统媒体键。
- `volume <0..1>`：设置进程音量（小数）；越界值截断到 `[0, 1]`。
- `seek <ms>`：请求跳转到指定毫秒；不可派发时失败。
- 裸执行与 `--help` 均打印用法。

### `status` 单行字段

```json
{"mode":"netease","t":0.0,"client":true,"source":"memory","state":"playing","title":"曲名","artist":"歌手","position_ms":61234,"duration_ms":215000,"volume":0.60,"exact":true,"line":12,"line_text":"当前歌词","line_tr":"译文","lyric_song_id":1234567,"lyric_lines":48,"synced":true,"lyric_origin":"webdb+remote","pending":false,"status":"memory · 48 lines · webdb+remote"}
```

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `mode` | string | 恒为 `"netease"` |
| `t` | number | 距 `status` 启动的秒数（1 位小数） |
| `client` | bool | 是否找到 `cloudmusic.exe` 窗口 |
| `source` | string | `"memory"` \| `"rpc"` \| `"seek-pending"` \| `"title+clock"` \| `"none"` |
| `state` | string | `"playing"` \| `"paused"` \| `"unknown"` |
| `title` / `artist` | string | 当前曲目 |
| `position_ms` | number | 最佳已知位置（毫秒，整数）；暂停时冻结 |
| `duration_ms` | number | 曲目总长（毫秒，整数） |
| `volume` | number | 进程音量，2 位小数 |
| `exact` | bool | 是否由内存或 RPC 实时锚定（时钟路径为 `false`） |
| `line` | int | 当前歌词行下标，`-1` 表示无 |
| `line_text` / `line_tr` | string | 当前歌词原文 / 译文 |
| `lyric_song_id` | int64 | 歌词对应的网易云歌曲 id |
| `lyric_lines` | int | 歌词行数 |
| `synced` | bool | 是否同步歌词（`false` 即 unsynced） |
| `lyric_origin` | string | `"rpc+remote"` \| `"webdb+remote"` \| `"rpc+cache"` \| `"none"` |
| `pending` | bool | 是否仍有后台任务（歌词/封面/seek）在途 |
| `status` | string | 诊断串，同 `Controller::status()`，如 `memory · 48 lines · webdb+remote` |

### 单次应答

| 命令 | 输出 |
| --- | --- |
| `title` | `{"mode":"title","found":bool,"title":string}` |
| `next` / `prev` / `toggle` | `{"mode":"next","ok":true}`（`mode` 为实际命令） |
| `volume` | `{"mode":"volume","ok":bool,"volume":number,"muted":bool}` |
| `seek` | `{"mode":"seek","ok":bool,"position_ms":number}`（`position_ms` 为请求的目标位置） |

### 退出码

`0` 成功；`1` 动作被拒绝 / 无结果（seek 无法派发、音量写入被拒、`title` 找不到客户端窗口）；`2` 用法错误。

## 库 API

```cpp
#include "ncm.h"

ncm::Config cfg;                 // cache_dir / rpc_base / audio_gate
ncm::Controller ctl;
ctl.configure(cfg);
ctl.update(dt);                  // 每帧调用，内部限流，不阻塞
```

`ncm::Controller`：

- `configure(const Config&)`
- `update(double dt)`
- `state()` → `const PlaybackState&`
- `lyrics()` → `LyricDoc`（互斥保护的快照）
- `lyrics_pending()` → `bool`
- `line_at(double position_ms)` → 当前应显示的歌词下标（无则 `-1`）
- `nudge(double delta_ms)` → 手动校时
- `volume()` / `is_muted()`
- `set_volume(float)` / `adjust_volume(float)` / `set_muted(bool)` / `toggle_mute()`
- `next_track()` / `prev_track()` / `toggle_play_pause()`
- `seek_ms(double target_ms)`
- `status()` → 一行诊断串

`ncm::netease_window_title()` 返回当前网易云窗口标题（客户端未运行时为空串）。结构体保持原名：`PlayState`、`TrackInfo`、`LyricLine`、`LyricDoc`、`PlaybackState`。

`Config`：`cache_dir`（默认 `cache`）、`rpc_base`（空则探测 `http://127.0.0.1:47251`）、`audio_gate`（默认 `true`，无发声会话时冻结回退时钟）。

## 限制

- 内存偏移绑定单一客户端构建（3.1.46 x64 / Build 205627 / patch 5d77cd4）；客户端更新后该路径失效，会自动降级到 RPC 或时钟，其他版本 best-effort。
- RPC 可选，需自行运行兼容 CloudMusic Local API 的只读服务；不提供时纯靠内存或时钟。
- 曲目标题是启发式：标题必须形如 `<歌曲> - <歌手>`；无 `" - "` 的标题不会被当作切歌动作，客户端 UI 文案/命名变化会影响识别。
- 位置 `exact` 仅在内存或 RPC 实时锚定时为 `true`；时钟路径无法感知 seek / 重播造成的跳变。
- 隐藏、最小化或被其他窗口遮挡的播放器无法 seek，本库不会为了跳转而打扰窗口。
- 音量与静音是进程级（`cloudmusic.exe` 的音频会话），不是系统全局音量。

## 来源

`src/ncm.h`、`src/ncm.cpp` 为 `vibe-mode` 中 `src/data/netease.{h,cpp}` 的逐字节等价迁移（仅命名改动），因此行为与原 HUD 完全一致。HUD 的自定义配色 / 主题逻辑仍留在 `vibe-mode`，未随本库迁移。

## 许可

MIT，见 [LICENSE](LICENSE)。
