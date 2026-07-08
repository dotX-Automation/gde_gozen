# MediaPlayback — threaded media player for the HMI

`media_playback.gd` (`class_name MediaPlayback`, extends `Control`) plays a media source with **one decode worker thread per instance**, so several live RTSP streams can play at once without stalling the main thread.
`video_yuva_to_rgba.gdshader` is the YUVA→RGBA canvas shader it drives.

## Installing into a Godot project

1. Copy `media_playback.gd` and `video_yuva_to_rgba.gdshader` into the project (e.g. `res://addons/gde_gozen/`), alongside the gde_gozen GDExtension.
2. Set the `SHADER_PATH` constant at the top of `media_playback.gd` to wherever you placed the shader.
3. Add a `MediaPlayback` node and call `open("rtsp://…")` or `open("res://clip.mp4")`.

Color is handled for every renderer automatically: the shader's `output_linear` uniform is set at setup, decoding the video to linear only under the **Forward+** renderer with **HDR 2D** enabled (where the canvas is composited in linear space) and leaving it untouched everywhere else.
No per-project shader swap is needed.

## Usage

```gdscript
var player := MediaPlayback.new()
add_child(player)
player.autoplay = true
player.network_timeout = 5.0                # seconds a blocking open/read may stall
                                            # before the stream is treated as dead (0 = infinite)
player.open("rtsp://mediamtx.local/cam0")   # live: latency-first, latest-wins
# player.open("res://clip.mp4")             # finite: seek/loop/duration available
```

On a live source both the video and audio paths **self-heal**: a stalled/dropped connection is detected within `network_timeout`, `media_disconnected` fires, and the player retries (up to `RECONNECT_MAX_ATTEMPTS`, `RECONNECT_BACKOFF_MSEC` apart).
It emits `media_reconnected` on recovery, or `media_error` after giving up.
Keep `network_timeout > 0` so a dead-from-start open cannot stall `close()` — see the `@export` note in the script.

Signals: `media_opened`, `media_closed`, `media_ended` (finite), `media_disconnected` / `media_reconnected` (live), `media_error(message)`, `playback_started`, `playback_paused`, `frame_changed(frame_nr)`.

Finite-only API: `seek_frame(n)`, `loop`, `speed`, `duration`, `elapsed`.
Catalog: `video_streams`, `audio_streams`, `subtitle_streams`, `get_stream_title(i)`, `get_stream_language(i)`, `get_chapter_count()`, `get_chapter(i)`.
Audio (works live too): `set_audio_stream(i)` — switch track (`-1` = default), reopened off the main thread so it never blocks the UI; returns `true` if the switch was dispatched.
`resync_audio()` — reopen the current audio track to drop accumulated live latency while video keeps running.

## Known limitations

- **Live A/V sync is best-effort.** Video and audio are two independent connections; there is no active resync (see the design doc).
  Latency stays bounded by video latest-wins and the audio ring buffer.
- **Live reconnect assumes an unchanged resolution.** A mid-stream resolution change would mismatch the preallocated textures.
- **`network_timeout = 0` (infinite) can stall `close()`** only for a source that is dead *before* the decoder is published (initial metadata/video/audio probe).
  A published decode is always cancellable; keep the timeout `> 0` to bound the startup case too.
