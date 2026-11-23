# GDE GoZen

GDE GoZen is a GDExtension addon that plays video and audio inside Godot through a statically linked FFmpeg. This build is the dotX Automation s.r.l. fork of [VoylinsGamedevJourney/gde_gozen](https://github.com/VoylinsGamedevJourney/gde_gozen), tuned for an HMI whose main workload is live RTSP video with optional audio. Local media files are supported as well.

## Installation

Copy this `gde_gozen` folder into your project's `addons/` folder and reopen the project. Copy the whole folder, sidecars included: `media_playback.gd.uid`, `video_yuva_to_rgba.gdshader.uid`, `gozen.gdextension.uid` and `icon.svg.import` carry fixed UIDs, so scenes that reference the addon keep resolving without Godot generating new UIDs and rewriting them.

## `MediaPlayback`

`MediaPlayback` (`media_playback.gd`) is the front-end: a `Control` node that opens a path or URL, decodes it on its own worker thread, and renders the frames through `video_yuva_to_rgba.gdshader`. Each instance has its own decode thread, so several live streams can play at once without stalling the main thread.

```gdscript
var player := MediaPlayback.new()
add_child(player)
player.open("rtsp://mediamtx.local/cam0")  # live; or "res://clip.mp4" for a finite file
```

A source counts as **live** when its video decoder reports no frame count (`frame_count <= 0`), which is the case for RTSP and other streams without a known duration. Each mode behaves differently:

- **Live**: latency first. Video shows the latest decoded frame and drops the older ones, and there is no active A/V resync. When a read drops, the node reconnects on its own and emits `media_disconnected` / `media_reconnected` for the picture (no `frame_changed` in between, and the textures follow a new resolution) and `audio_disconnected` / `audio_reconnected` for the sound. `network_timeout`, `reconnect_max_attempts` and `reconnect_delay` tune this; by default it retries forever. `close()` cancels any open or read in flight, so it never waits out a timeout.
- **Finite**: framerate pacing, `seek_frame()`, `loop`, `speed`, and audio-follow sync (`audio_speed_to_sync`). `media_ended` fires when the last frame has been shown.

On a live source, video and audio arrive over two independent connections with no shared clock, so live A/V sync is best-effort. The latency stays bounded by the latest-wins video and the audio ring buffer. `set_audio_stream()` switches the audio track without blocking the main thread, and `resync_audio()` re-aligns it. Press F1 in the editor and search for `MediaPlayback` to read the full class reference, which comes from the script's doc comments.

## Native classes

Four classes are registered, and they share one FFmpeg core:

- `GoZenVideo` (`Resource`): a pull-based video frame decoder (`open`, `next_frame`, `seek_frame`, then the `get_y/u/v/a_data()` planes). It exposes only playback and render properties, such as the render resolution, framerate, frame count and rotation. `get_media_path()` returns the opened path.
- `AudioStreamFFmpeg`: an `AudioStream`, so the audio server pulls and mixes it on the audio thread. This is the streaming audio path, and the one used for RTSP.
- `GoZenAudio` (`Resource`): an eager batch decoder that returns S16 PCM or an `AudioStreamWAV`. It works on finite sources only and rejects live ones.
- `GoZenMetadata` (`Resource`): a one-shot probe of the media catalog, covering video, audio and subtitle streams, per-stream codec, title, language and metadata, intrinsics and chapters. It opens the source, reads everything, and closes the connection right away.

The split works like this: the decoders expose what they need to play and render their stream, and `GoZenMetadata` owns all catalog and info queries. On a network source, each decoder sets up only its own track.

## Compatibility and platforms

`compatibility_minimum` is `4.7`. The target runtime is Godot 4.7.2, and the addon also loads on Godot 4.8 dev builds.

`bin/` ships builds for these platforms:

- Linux: x86_64 and arm64
- Windows: x86_64 and x86_32
- Android: arm64 and arm32

`gozen.gdextension` also declares `macos.*` and `web.*` libraries, but `bin/` doesn't include them. macOS isn't built because there is no Mac to build on. Web is left out for now because of its reduced feature set. Either one may be supported later.

## Media files in the project

To see video files in the FileSystem dock, add their extensions (for example `mp4`) to the editor setting `docks/filesystem/other_file_extensions`. Exports leave media files out unless you add their extensions (for example `*.mp4`) to each export preset's non-resource export filter. The exported executable needs the GDE GoZen library next to it.

On Android, paths from the SAF file picker may not be absolute. Copy the file to a temporary folder to get an absolute path, and delete the copy afterwards.

> This software uses libraries from the FFmpeg project under the LGPLv2.1.
