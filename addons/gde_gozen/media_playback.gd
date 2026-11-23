@icon("res://addons/gde_gozen/icon.svg")
class_name MediaPlayback
extends Control
## Threaded media player for live RTSP streams and finite media files.
##
## One worker Thread per instance owns the GoZenVideo decoder and hands each decoded frame to a
## mutex-guarded slot. The main thread uploads it through the RenderingServer, drives the
## AudioStreamPlayer and emits every signal.
## A live source plays latest-frame-wins and reconnects on its own. A finite one adds frame-rate
## pacing, seek, loop and audio-follow sync.
## The media catalog is read from a one-shot GoZenMetadata probe.
##
## Every open() resolves exactly once, by media_opened or media_error, unless close() or another
## open() comes first. An open that close() interrupted emits nothing.
## The media_* signals report the picture, or the whole source before it opens. The audio_* signals
## report the sound track alone.
## Live A/V sync is best-effort: video and audio are independent connections with no shared clock,
## and latency stays bounded by the latest-wins video and the audio ring buffer.
## URLs in messages carry no user information.
##
## Usage:
## [codeblock]
## var player := MediaPlayback.new()
## add_child(player)
## player.open("rtsp://mediamtx.local/cam0")  # Or a finite source, such as "res://clip.mp4".
## [/codeblock]

#region Signals
## The source opened, with its first frame ready when it has a picture. is_ready is true from here.
signal media_opened
## close() released the source.
signal media_closed
## A finite source without loop presented its last frame, and playback stopped.
signal media_ended
## A live source lost the picture. No frame_changed follows until media_reconnected.
signal media_disconnected
## A live source got the picture back, possibly at another resolution.
signal media_reconnected
## open() failed, or a live picture stayed lost past reconnect_max_attempts. message names the cause.
signal media_error(message: String)
## A live source lost the sound, which reconnects on its own.
signal audio_disconnected
## A live source got the sound back.
signal audio_reconnected
## set_audio_stream() failed, or the live sound stayed lost past reconnect_max_attempts.
signal audio_error(message: String)
## play() started playback.
signal playback_started
## pause() paused playback.
signal playback_paused
## A new frame was presented. frame_nr is its index.
signal frame_changed(frame_nr: int)
#endregion


#region Enums
## YUV to RGB matrix. AUTO follows the stream's color primaries.
enum COLOR_PROFILE { AUTO, BT470, BT601, BT709, BT2020, BT2100 }
#endregion


#region Constants
## Shader converting the YUV(A) planes to RGBA.
const SHADER_PATH: String = "res://addons/gde_gozen/video_yuva_to_rgba.gdshader"
## A/V offset of a finite source, in seconds, past which the audio seeks to the video.
const AUDIO_OFFSET_THRESHOLD: float = 0.1
## Worker sleep while paused, and step of its reconnect sleep, in milliseconds.
const PAUSE_POLL_MSEC: int = 10
#endregion


#region Exported Variables
@export_category("Media")
## Calls play() once the source opens.
@export var autoplay: bool = false
## Finite sources only: restarts from frame 0 at the end.
@export var loop: bool = false
## Finite sources only: playback speed factor.
@export_range(0.25, 4.0, 0.05) var speed: float = 1.0
## Seconds a blocking open or read may stall before the stream counts as dead, 0 for infinite.
## close() cancels any open or read in flight either way.
@export var network_timeout: float = 5.0

@export_group("Live Reconnect")
## Live sources only: reconnect attempts after a failed read, before media_error or audio_error.
## -1 retries until the source returns or close() runs, 0 makes the first failure fatal.
@export var reconnect_max_attempts: int = -1
## Live sources only: seconds between reconnect attempts.
@export var reconnect_delay: float = 0.5

@export_group("Video")
## Decodes the picture. Read by open().
@export var video_enable: bool = true
## YUV to RGB matrix, replacing the stream's own when not AUTO.
@export var video_color_profile: COLOR_PROFILE = COLOR_PROFILE.AUTO

@export_group("Audio")
## Plays the sound track.
@export var audio_enable: bool = true
## Index of the audio stream to play, -1 for the default one. set_audio_stream() writes it.
@export var audio_stream: int = -1
## Finite sources only: nudges the audio pitch by 1% to close an A/V offset under
## AUDIO_OFFSET_THRESHOLD.
@export var audio_speed_to_sync: bool = false
## Keeps the audio pitch unchanged at a speed other than 1.
@export var audio_pitch_adjust: bool = true

@export_category("Debug")
## Turns on GoZenVideo's debug log, which also raises FFmpeg's global log level to verbose.
@export var debug: bool = false
#endregion


#region Public State (read-only)
## TextureRect showing the picture, created by _enter_tree() and kept across reconnects.
var video_texture: TextureRect = null
## Player of the sound, on a bus of its own carrying the pitch-shift effect.
var audio_player: AudioStreamPlayer = null

## Source of the last open(), with a uid:// resolved to its path.
var path: String:
	get: return _path
## True from open() until close().
var is_open: bool:
	get: return _is_open
## True from media_opened until close().
var is_ready: bool:
	get: return _is_ready
var is_playing: bool:
	get: return _is_playing
## True for a source without a frame count, such as an RTSP stream.
var is_live: bool:
	get: return _is_live

## Decoded picture size in pixels.
var resolution: Vector2i:
	get: return _resolution
## Frames per second, 0 without a picture.
var frame_rate: float:
	get: return _frame_rate
## Frame count, 0 or less for a live source.
var frame_count: int:
	get: return _frame_count
## Index of the last presented frame.
var current_frame: int:
	get: return _current_frame
## Display rotation of the picture, in degrees.
var frame_rotation: int:
	get: return _rotation
## True when the picture carries an alpha plane.
var has_alpha: bool:
	get: return _has_alpha

## Duration in seconds, 0 when unknown.
var duration: float:
	get: return (_duration_us / 1_000_000.0) if _duration_us > 0 else 0.0
## Position of the last presented frame, in seconds.
var elapsed: float:
	get: return (_current_frame / _frame_rate) if _frame_rate > 0.0 else 0.0

## Absolute indices of the source's video, audio and subtitle streams, empty before the source
## opens.
var video_streams: PackedInt32Array:
	get: return _metadata.get_video_streams() if _metadata != null else PackedInt32Array()
var audio_streams: PackedInt32Array:
	get: return _metadata.get_audio_streams() if _metadata != null else PackedInt32Array()
var subtitle_streams: PackedInt32Array:
	get: return _metadata.get_subtitle_streams() if _metadata != null else PackedInt32Array()
#endregion


#region Private Variables
## Guards the state shared with the worker and the audio open tasks.
var _mutex: Mutex = Mutex.new()
## Decode worker of the current open().
var _thread: Thread = Thread.new()

var _path: String = ""
## Worker-owned decoder, published and cleared under _mutex for close() to cancel().
var _video: GoZenVideo = null
## Metadata, decoder or audio stream whose open() the worker is running, published under _mutex for
## close() to cancel().
var _opening: Object = null
## Count of open() calls, tagging the worker's deferred calls with their session.
var _session: int = 0
## Audio stream playing in audio_player.
var _audio: AudioStreamFFmpeg = null
## Catalog probe of the source, written once by the worker and read-only after.
var _metadata: GoZenMetadata = null

var _shader_material: ShaderMaterial = null
## Pitch-shift effect on audio_player's bus.
var _audio_pitch_effect: AudioEffectPitchShift = null
## Textures of the Y, U, V and A planes, sampled by the shader.
var _y_tex: ImageTexture = null
var _u_tex: ImageTexture = null
var _v_tex: ImageTexture = null
var _a_tex: ImageTexture = null

## Frame handoff slot, guarded by _mutex: the worker's latest planes, their frame index, and
## whether the main thread has yet to present them.
var _slot_y: Image = null
var _slot_u: Image = null
var _slot_v: Image = null
var _slot_a: Image = null
var _slot_frame: int = 0
var _slot_dirty: bool = false

## Worker commands, guarded by _mutex: keep running, stay paused, and the frame to seek to
## (-1 for none).
var _running: bool = false
var _paused: bool = true
var _seek_target: int = -1

## Generation of the latest audio open request, guarded by _mutex. A result of an older one is
## dropped.
var _audio_open_gen: int = 0
## Audio stream whose open() is in flight, guarded by _mutex for close() to cancel().
var _audio_pending: AudioStreamFFmpeg = null
## WorkerThreadPool task of the audio open in flight, -1 for none. Guarded by _mutex.
var _audio_task_id: int = -1

## A live audio reconnect cycle is running. Main thread only, as are the four below.
var _audio_reconnecting: bool = false
## Attempts made in the current reconnect cycle.
var _audio_reconnect_attempts: int = 0
## Earliest Time.get_ticks_msec() of the next reconnect attempt.
var _audio_reconnect_next_ms: int = 0
## A reopen dispatched by the reconnect cycle has yet to report its result.
var _audio_attempt_inflight: bool = false
## The live sound was given up on, until the next open() or a successful reopen.
var _audio_lost: bool = false

var _is_open: bool = false
var _is_ready: bool = false
var _is_playing: bool = false
var _is_live: bool = true
## True between media_disconnected and media_reconnected, while no frame is presented.
var _video_lost: bool = false

var _resolution: Vector2i = Vector2i.ZERO
var _frame_rate: float = 0.0
var _frame_count: int = 0
var _current_frame: int = 0
var _rotation: int = 0
var _has_alpha: bool = false
var _duration_us: int = 0
#endregion


#region Built-in Methods
## Builds video_texture with the YUV shader, and audio_player on a new bus carrying the pitch-shift
## effect.
func _enter_tree() -> void:
	_shader_material = ShaderMaterial.new()
	_shader_material.shader = load(SHADER_PATH)
	_shader_material.set_shader_parameter("output_linear", _needs_linear_output())

	video_texture = TextureRect.new()
	video_texture.material = _shader_material
	video_texture.texture = ImageTexture.new()
	video_texture.anchor_right = Control.ANCHOR_END
	video_texture.anchor_bottom = Control.ANCHOR_END
	video_texture.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	video_texture.expand_mode = TextureRect.EXPAND_IGNORE_SIZE
	add_child(video_texture)

	_audio_pitch_effect = AudioEffectPitchShift.new()
	AudioServer.add_bus()
	AudioServer.add_bus_effect(AudioServer.bus_count - 1, _audio_pitch_effect)

	audio_player = AudioStreamPlayer.new()
	audio_player.bus = AudioServer.get_bus_name(AudioServer.bus_count - 1)
	add_child(audio_player)


## Closes the source and removes audio_player's bus.
func _exit_tree() -> void:
	close()
	if audio_player != null:
		AudioServer.remove_bus(AudioServer.get_bus_index(audio_player.bus))


## Closes the source on deletion, including for a node that never entered the tree.
func _notification(what: int) -> void:
	if what == NOTIFICATION_PREDELETE:
		close()


## Presents the latest frame and polls the health of the live sound.
func _process(_delta: float) -> void:
	if not _is_open:
		return

	if not _video_lost:
		_present()

	if audio_enable and _audio != null and _is_playing and _is_live and not _audio_lost:
		_poll_audio_health()


## Uploads the worker's latest frame, if a new one is waiting, and emits frame_changed.
## On a finite source the audio then follows the frame.
func _present() -> void:
	var presented := false
	_mutex.lock()
	if _slot_dirty:
		_slot_dirty = false
		if _y_tex != null:
			RenderingServer.texture_2d_update(_y_tex.get_rid(), _slot_y, 0)
			RenderingServer.texture_2d_update(_u_tex.get_rid(), _slot_u, 0)
			RenderingServer.texture_2d_update(_v_tex.get_rid(), _slot_v, 0)
			if _has_alpha and _slot_a != null:
				RenderingServer.texture_2d_update(_a_tex.get_rid(), _slot_a, 0)
		_current_frame = _slot_frame
		presented = true
	_mutex.unlock()

	if presented:
		frame_changed.emit(_current_frame)
		if not _is_live and audio_enable and _audio != null and _is_playing:
			_sync_audio_video()
#endregion


#region Public Methods
## Closes any current source and opens media_path, a path, uid:// or URL, on a new worker.
## Resolves by media_opened or media_error.
func open(media_path: String = _path) -> void:
	close()

	if media_path.begins_with("uid://"):
		media_path = ResourceUID.get_id_path(ResourceUID.text_to_id(media_path))

	_path = media_path
	_reset_state()
	_session += 1
	_is_open = true
	_running = true
	_paused = true
	_thread = Thread.new()
	_thread.start(_open_and_decode)


## Starts or resumes playback. Called before media_opened, it takes effect once the source opens.
func play() -> void:
	if not _is_open or _is_playing:
		return
	_is_playing = true
	_mutex.lock()
	_paused = false
	_mutex.unlock()

	_start_audio_playback()

	playback_started.emit()


## Pauses playback.
func pause() -> void:
	if not _is_open or not _is_playing:
		return
	_is_playing = false
	_mutex.lock()
	_paused = true
	_mutex.unlock()

	if audio_enable and audio_player.stream != null:
		audio_player.set_stream_paused(true)

	playback_paused.emit()


## Finite sources only: seeks to frame_nr, clamped to the frame count, and the audio follows.
## The worker applies the seek.
func seek_frame(frame_nr: int) -> void:
	if not _is_open or _is_live:
		return
	var target := clampi(frame_nr, 0, _frame_count)
	_mutex.lock()
	_seek_target = target
	_mutex.unlock()


## Switches to audio stream stream_index, -1 for the default, opened on a WorkerThreadPool task.
## A switch in flight is cancelled first. False when nothing is open, audio is disabled or the
## index names no audio stream. A failed open outside a reconnect cycle emits audio_error.
func set_audio_stream(stream_index: int) -> bool:
	if not _is_open or not audio_enable:
		return false
	if stream_index != -1 and _metadata != null and not _metadata.get_audio_streams().has(stream_index):
		push_error("MediaPlayback: invalid audio stream %d" % stream_index)
		return false

	audio_stream = stream_index

	_mutex.lock()
	_audio_open_gen += 1
	var gen := _audio_open_gen
	var prev := _audio_pending
	var prev_task := _audio_task_id
	_audio_pending = null
	_audio_task_id = -1
	_mutex.unlock()

	if prev != null:
		prev.cancel()
	if prev_task != -1:
		WorkerThreadPool.wait_for_task_completion(prev_task)

	var tid := WorkerThreadPool.add_task(_open_audio_task.bind(stream_index, gen))
	_mutex.lock()
	_audio_task_id = tid
	_mutex.unlock()
	return true


## Reopens the current audio stream off the main thread while the video keeps running.
## A live sound then lands at the present, a finite one at the current frame.
func resync_audio() -> void:
	if not _is_open or not audio_enable or _audio == null:
		return
	set_audio_stream(audio_stream)


## Cancels any open or read in flight, joins the worker and the audio open task, and emits
## media_closed. A no-op when nothing is open. Never waits out a network_timeout.
## It waits out the current audio mix period, on which the AudioServer frees the stopped playback.
func close() -> void:
	if not _is_open:
		return
	_is_open = false
	_is_ready = false
	_is_playing = false
	_mutex.lock()
	_running = false
	_paused = false
	var v: GoZenVideo = _video
	var opening: Object = _opening
	_mutex.unlock()

	if v != null:
		v.cancel()
	if opening != null:
		opening.cancel()

	if _thread.is_started():
		_thread.wait_to_finish()

	_mutex.lock()
	_audio_open_gen += 1
	var pending_audio: AudioStreamFFmpeg = _audio_pending
	var pending_task: int = _audio_task_id
	_audio_pending = null
	_audio_task_id = -1
	_mutex.unlock()
	if pending_audio != null:
		pending_audio.cancel()
	if pending_task != -1:
		WorkerThreadPool.wait_for_task_completion(pending_task)

	_audio_reconnecting = false
	_audio_reconnect_attempts = 0
	_audio_reconnect_next_ms = 0
	_audio_attempt_inflight = false
	_audio_lost = false

	if audio_player != null:
		var had_playback := audio_player.has_stream_playback()
		audio_player.stop()
		audio_player.stream = null
		if had_playback:
			OS.delay_usec(maxi(0, int(AudioServer.get_time_to_next_mix() * 1_000_000.0)) + 1000)
	_audio = null
	_metadata = null
	_current_frame = 0
	_video_lost = false

	media_closed.emit()


## Title of stream stream_index, empty before the source opens.
func get_stream_title(stream_index: int) -> String:
	return _metadata.get_stream_title(stream_index) if _metadata != null else ""


## Language of stream stream_index, empty before the source opens.
func get_stream_language(stream_index: int) -> String:
	return _metadata.get_stream_language(stream_index) if _metadata != null else ""


## Chapter count, 0 before the source opens.
func get_chapter_count() -> int:
	return _metadata.get_chapter_count() if _metadata != null else 0


## Chapter chapter_index, empty before the source opens.
func get_chapter(chapter_index: int) -> Chapter:
	var c := Chapter.new()
	if _metadata != null:
		c.start = _metadata.get_chapter_start_us(chapter_index) / 1_000_000.0
		c.end = _metadata.get_chapter_end_us(chapter_index) / 1_000_000.0
		c.title = str(_metadata.get_chapter_metadata(chapter_index).get("title", ""))
	return c
#endregion


#region Worker Thread
## Worker entry: opens the metadata, video and audio in turn, posts the result, then decodes until
## close(). Each blocking open() is published first for close() to cancel().
## The metadata opens first: it picks the tracks to open, and an unreachable source fails on it
## after one network_timeout.
func _open_and_decode() -> void:
	var meta := GoZenMetadata.new()
	meta.set_network_timeout(network_timeout)
	if not _set_opening(meta) or meta.open(_path) != OK:
		_post(_notify_error.bind("Cannot open: %s" % _redact(_path)))
		return
	_metadata = meta

	if video_enable and not meta.get_video_streams().is_empty():
		var v := GoZenVideo.new()
		if debug:
			v.enable_debug()
		v.set_network_timeout(network_timeout)
		if not _set_opening(v) or v.open(_path) != OK:
			_post(_notify_error.bind("Cannot open video: %s" % _redact(_path)))
			return
		_mutex.lock()
		_video = v
		_opening = null
		_mutex.unlock()
		if not _video.next_frame(false):
			_post(_notify_error.bind("Cannot decode first frame: %s" % _redact(_path)))
			return

	if audio_enable and not meta.get_audio_streams().is_empty():
		var a := AudioStreamFFmpeg.new()
		a.set_network_timeout(network_timeout)
		if _set_opening(a) and a.open(_path, audio_stream) == OK:
			_audio = a
		_set_opening(null)

	if _video == null and _audio == null:
		_post(_notify_error.bind("Nothing to play: %s" % _redact(_path)))
		return

	var facts := {"is_live": meta.get_duration_us() <= 0, "duration_us": meta.get_duration_us()}
	if _video != null:
		_store_frame()
		facts = _video_facts()
	_post(_on_opened.bind(facts))

	if _video == null:
		return

	if facts.is_live:
		_run_live()
	else:
		_run_finite()

	_video.close()
	_mutex.lock()
	_video = null
	_mutex.unlock()


## Publishes obj as the object whose open() the worker is about to run, or null once done.
## False when close() already ran: its cancel() missed obj, whose open() must then not start.
func _set_opening(obj: Object) -> bool:
	_mutex.lock()
	_opening = obj
	var running := _running
	_mutex.unlock()
	return running


## Queues method on the main thread, tagged with the current session. The call is dropped when
## that session closed or was replaced by a new open() before it runs.
func _post(method: Callable) -> void:
	_deliver.call_deferred(_session, method)


## Runs method when session is still the open one.
func _deliver(session: int, method: Callable) -> void:
	if session == _session and _is_open:
		method.call()


## The decoder's playback facts, as _apply_facts() reads them.
func _video_facts() -> Dictionary:
	return {
		"resolution": _video.get_resolution(),
		"framerate": _video.get_framerate(),
		"rotation": _video.get_rotation(),
		"has_alpha": _video.has_alpha(),
		"full_color": _video.is_full_color_range(),
		"interlaced": _video.get_interlaced(),
		"color_profile": _video.get_color_profile(),
		"frame_count": _video.get_frame_count(),
		"duration_us": _video.get_duration_us(),
		"is_live": _video.get_frame_count() <= 0,
	}


## Live decode loop: stores every frame latest-wins. A failed read posts media_disconnected, then
## the decoder reopens every reconnect_delay until it reads again, close() runs or
## reconnect_max_attempts is spent. A reconnect re-reads the video facts.
func _run_live() -> void:
	var attempts := 0
	var disconnected := false
	while _get_running():
		if _get_paused():
			OS.delay_msec(PAUSE_POLL_MSEC)
			continue
		if _video.next_frame(false):
			attempts = 0
			_store_frame()
			if disconnected:
				disconnected = false
				_post(_notify_reconnected.bind(_video_facts()))
			continue
		if not _get_running():
			return
		if reconnect_max_attempts >= 0 and attempts >= reconnect_max_attempts:
			_post(_notify_error.bind("Live stream lost: %s" % _redact(_path)))
			return
		if not disconnected:
			disconnected = true
			_post(_notify_disconnected)
		attempts += 1
		_reconnect_sleep()
		_video.close()
		_video.open(_path)


## reconnect_delay in milliseconds, never negative.
func _reconnect_delay_ms() -> int:
	return maxi(0, int(reconnect_delay * 1000.0))


## Sleeps reconnect_delay on the worker in PAUSE_POLL_MSEC steps, returning early once close()
## clears _running.
func _reconnect_sleep() -> void:
	var remaining := _reconnect_delay_ms()
	while remaining > 0 and _get_running():
		var step := mini(remaining, PAUSE_POLL_MSEC)
		OS.delay_msec(step)
		remaining -= step


## Finite decode loop: paces frames at the frame rate times speed, skipping frames to catch up,
## and applies seeks, loop and the end of the source.
func _run_finite() -> void:
	var fr := _video.get_framerate()
	if fr <= 0.0:
		fr = 30.0
	var last := Time.get_ticks_usec()
	var acc := 0
	while _get_running():
		var target := _take_seek()
		if target >= 0:
			if _video.seek_frame(target) == OK:
				_store_frame()
				_post(_seek_audio.bind(target))
			last = Time.get_ticks_usec()
			acc = 0
			continue

		if _get_paused():
			OS.delay_msec(PAUSE_POLL_MSEC)
			last = Time.get_ticks_usec()
			continue

		var frame_time := int((1_000_000.0 / fr) / maxf(speed, 0.05))
		var now := Time.get_ticks_usec()
		acc += now - last
		last = now
		if acc < frame_time:
			OS.delay_usec(mini(frame_time - acc, 5000))
			continue

		@warning_ignore("integer_division")
		var skips := acc / frame_time
		acc -= skips * frame_time

		var eof := false
		while skips > 1:
			if not _video.next_frame(true):
				eof = true
				break
			skips -= 1
		if not eof and not _video.next_frame(false):
			eof = true

		if eof:
			if loop:
				if _video.seek_frame(0) == OK:
					_store_frame()
					_post(_seek_audio.bind(0))
				last = Time.get_ticks_usec()
				acc = 0
			else:
				_post(_notify_ended)
				_mutex.lock()
				_paused = true
				_mutex.unlock()
			continue

		_store_frame()


## Copies the decoder's planes into the handoff slot and marks it for presenting.
func _store_frame() -> void:
	_mutex.lock()
	if _slot_y == null:
		_slot_y = Image.new()
		_slot_u = Image.new()
		_slot_v = Image.new()
	_slot_y.copy_from(_video.get_y_data())
	_slot_u.copy_from(_video.get_u_data())
	_slot_v.copy_from(_video.get_v_data())
	if _video.has_alpha():
		if _slot_a == null:
			_slot_a = Image.new()
		_slot_a.copy_from(_video.get_a_data())
	_slot_frame = _video.get_current_frame()
	_slot_dirty = true
	_mutex.unlock()


## _running, read under _mutex.
func _get_running() -> bool:
	_mutex.lock()
	var r := _running
	_mutex.unlock()
	return r


## _paused, read under _mutex.
func _get_paused() -> bool:
	_mutex.lock()
	var p := _paused
	_mutex.unlock()
	return p


## Returns the pending seek target, -1 for none, and clears it.
func _take_seek() -> int:
	_mutex.lock()
	var t := _seek_target
	_seek_target = -1
	_mutex.unlock()
	return t
#endregion


#region Main-thread Notifications (posted by the worker through _post)
## Applies the opened source's facts, attaches its audio and emits media_opened.
## Starts the audio of a play() that came first, and plays on autoplay.
func _on_opened(facts: Dictionary) -> void:
	_apply_facts(facts)

	if _audio != null:
		audio_player.stream = _audio
		if _is_playing:
			_start_audio_playback()

	_is_ready = true
	media_opened.emit()

	if autoplay:
		play()


## Stores the source facts and rebuilds the plane textures from the frame in the slot.
## video_texture, its texture and its material stay the same objects across reconnects.
func _apply_facts(facts: Dictionary) -> void:
	_is_live = facts.get("is_live", true)
	_resolution = facts.get("resolution", Vector2i.ZERO)
	_frame_rate = facts.get("framerate", 0.0)
	_rotation = facts.get("rotation", 0)
	_has_alpha = facts.get("has_alpha", false)
	_frame_count = facts.get("frame_count", 0)
	_duration_us = facts.get("duration_us", 0)

	if facts.has("resolution"):
		_mutex.lock()
		_y_tex = ImageTexture.create_from_image(_slot_y)
		_u_tex = ImageTexture.create_from_image(_slot_u)
		_v_tex = ImageTexture.create_from_image(_slot_v)
		if _has_alpha:
			_a_tex = ImageTexture.create_from_image(_slot_a)
		_mutex.unlock()

		if not _has_alpha:
			var white := Image.create_empty(maxi(_resolution.x, 1), maxi(_resolution.y, 1), false, Image.FORMAT_R8)
			white.fill(Color.WHITE)
			_a_tex = ImageTexture.create_from_image(white)

		var bg_w := _resolution.x
		var bg_h := _resolution.y
		if absi(_rotation) % 180 == 90:
			bg_w = _resolution.y
			bg_h = _resolution.x
		var bg := Image.create_empty(maxi(bg_w, 1), maxi(bg_h, 1), false, Image.FORMAT_R8)
		bg.fill(Color.WHITE)
		(video_texture.texture as ImageTexture).set_image(bg)

		_shader_material.set_shader_parameter("resolution", Vector2(_resolution))
		_shader_material.set_shader_parameter("full_color", facts.get("full_color", true))
		_shader_material.set_shader_parameter("interlaced", facts.get("interlaced", 0))
		_shader_material.set_shader_parameter("rotation", deg_to_rad(float(_rotation)))
		_shader_material.set_shader_parameter("color_profile", _resolve_color_profile(str(facts.get("color_profile", ""))))
		_shader_material.set_shader_parameter("y_data", _y_tex)
		_shader_material.set_shader_parameter("u_data", _u_tex)
		_shader_material.set_shader_parameter("v_data", _v_tex)
		_shader_material.set_shader_parameter("a_data", _a_tex)


## Pushes message as a warning and emits media_error.
func _notify_error(message: String) -> void:
	push_warning("MediaPlayback: " + message)
	media_error.emit(message)


## Pushes message as a warning and emits audio_error.
func _notify_audio_error(message: String) -> void:
	push_warning("MediaPlayback: " + message)
	audio_error.emit(message)


## Presents the frame the worker stored before its failed read, then stops presenting and emits
## media_disconnected.
func _notify_disconnected() -> void:
	_present()
	_video_lost = true
	media_disconnected.emit()


## Resumes presenting with the reconnected source's facts and emits media_reconnected.
func _notify_reconnected(facts: Dictionary) -> void:
	_video_lost = false
	_apply_facts(facts)
	media_reconnected.emit()


## Stops playback, pauses the audio and emits media_ended.
func _notify_ended() -> void:
	_is_playing = false
	if audio_enable and audio_player.stream != null:
		audio_player.set_stream_paused(true)
	media_ended.emit()


## Moves the audio of a finite source to frame frame_nr, paused unless playing.
func _seek_audio(frame_nr: int) -> void:
	if not audio_enable or audio_player.stream == null or _frame_rate <= 0.0:
		return
	if audio_player.stream.get_length() == 0.0:
		return
	audio_player.set_stream_paused(false)
	audio_player.play(frame_nr / _frame_rate)
	audio_player.set_stream_paused(not _is_playing)


## WorkerThreadPool task: opens audio stream stream_index and defers the result to the main
## thread. The blocking open() is its only work.
func _open_audio_task(stream_index: int, gen: int) -> void:
	var a := AudioStreamFFmpeg.new()
	a.set_network_timeout(network_timeout)

	_mutex.lock()
	if gen != _audio_open_gen:
		_mutex.unlock()
		_on_audio_open_result.call_deferred(false, gen)
		return
	_audio_pending = a
	_mutex.unlock()

	var err := a.open(_path, stream_index)

	_mutex.lock()
	if _audio_pending == a:
		_audio_pending = null
	var current := (gen == _audio_open_gen)
	_mutex.unlock()

	if err != OK:
		_on_audio_open_result.call_deferred(false, gen)
		return
	if not current:
		_on_audio_open_result.call_deferred(false, gen)
		return
	_apply_audio_stream.call_deferred(a, gen)
	_on_audio_open_result.call_deferred(true, gen)


## Installs a on audio_player unless a newer request or close() came after its open, and resumes
## it at the current position when playing.
func _apply_audio_stream(a: AudioStreamFFmpeg, gen: int) -> void:
	_mutex.lock()
	var current := (gen == _audio_open_gen)
	_mutex.unlock()
	if not current or not _is_open:
		return

	_audio = a
	audio_player.stream = _audio
	if _is_playing:
		audio_player.pitch_scale = speed
		_apply_pitch_adjust()
		audio_player.set_stream_paused(false)
		if _is_live:
			audio_player.play()
		else:
			audio_player.play((_current_frame / _frame_rate) if _frame_rate > 0.0 else 0.0)


## Detects a live sound latched unhealthy and drives its reconnect cycle, which follows the
## picture's: one attempt at a time, reconnect_delay apart, within reconnect_max_attempts.
func _poll_audio_health() -> void:
	if not _audio_reconnecting:
		if _audio.is_stream_healthy():
			return
		if reconnect_max_attempts == 0:
			_audio_lost = true
			_notify_audio_error("Live audio lost: %s" % _redact(_path))
			return
		_audio_reconnecting = true
		_audio_reconnect_attempts = 0
		_audio_reconnect_next_ms = 0
		audio_disconnected.emit()

	if _audio_attempt_inflight or Time.get_ticks_msec() < _audio_reconnect_next_ms:
		return
	if reconnect_max_attempts >= 0 and _audio_reconnect_attempts >= reconnect_max_attempts:
		_audio_reconnecting = false
		_audio_lost = true
		_notify_audio_error("Live audio lost: %s" % _redact(_path))
		return
	if set_audio_stream(audio_stream):
		_audio_reconnect_attempts += 1
		_audio_attempt_inflight = true


## Concludes an audio open, dropped when superseded. Success ends a reconnect cycle, a failure
## within one schedules the next attempt, and any other failure emits audio_error.
## Every open reports here, and the cycle is paced on these results.
func _on_audio_open_result(ok: bool, gen: int) -> void:
	_mutex.lock()
	var current := (gen == _audio_open_gen)
	_mutex.unlock()
	if not current:
		return

	_audio_attempt_inflight = false
	if ok:
		_audio_lost = false
		_audio_reconnect_attempts = 0
		_audio_reconnect_next_ms = 0
		if _audio_reconnecting:
			_audio_reconnecting = false
			audio_reconnected.emit()
	elif _audio_reconnecting:
		_audio_reconnect_next_ms = Time.get_ticks_msec() + _reconnect_delay_ms()
	else:
		_notify_audio_error("Cannot open audio stream %d" % audio_stream)
#endregion


#region Helpers (main thread)
## Clears the per-source state before a new open().
func _reset_state() -> void:
	_is_ready = false
	_is_playing = false
	_current_frame = 0
	_seek_target = -1
	_slot_dirty = false
	_slot_y = null
	_slot_u = null
	_slot_v = null
	_slot_a = null
	_y_tex = null
	_u_tex = null
	_v_tex = null
	_a_tex = null
	_audio_reconnecting = false
	_audio_reconnect_attempts = 0
	_audio_reconnect_next_ms = 0
	_audio_attempt_inflight = false
	_audio_lost = false
	_video_lost = false


## Returns url without its user information (user:password@). The last '@' before the path ends
## it, as in FFmpeg.
static func _redact(url: String) -> String:
	var host_start := url.find("://")
	if host_start < 0:
		return url
	host_start += 3
	var path_start := url.find("/", host_start)
	if path_start < 0:
		path_start = url.length()
	var at := url.rfind("@", path_start - 1)
	return url.substr(0, host_start) + url.substr(at + 1) if at >= host_start else url


## YUV to RGB coefficients of profile_str, or of video_color_profile when it is not AUTO.
## An unknown profile gets BT.709.
func _resolve_color_profile(profile_str: String) -> Vector4:
	var s := profile_str
	if video_color_profile != COLOR_PROFILE.AUTO:
		s = str(COLOR_PROFILE.find_key(video_color_profile)).to_lower()
	match s:
		"bt2020", "bt2100":
			return Vector4(1.4746, 0.16455, 0.57135, 1.8814)
		"bt601", "bt470", "bt470bg", "smpte170m":
			return Vector4(1.402, 0.344136, 0.714136, 1.772)
		_:
			return Vector4(1.5748, 0.1873, 0.4681, 1.8556)


## True on Forward+ with rendering/viewport/hdr_2d, the one setup compositing the 2D canvas in
## linear space. The shader's output is sRGB-encoded, and its output_linear uniform decodes it.
func _needs_linear_output() -> bool:
	return RenderingServer.get_current_rendering_method() == "forward_plus" \
			and bool(ProjectSettings.get_setting("rendering/viewport/hdr_2d", false))


## Starts the audio at the current position, with speed and pitch applied. A no-op without an
## audio stream. Called by play(), and by _on_opened() for a play() that came before the stream.
func _start_audio_playback() -> void:
	if not audio_enable or audio_player.stream == null:
		return
	audio_player.pitch_scale = speed
	_apply_pitch_adjust()
	audio_player.set_stream_paused(false)
	if _is_live:
		if not audio_player.playing:
			audio_player.play()
	else:
		audio_player.play((_current_frame / _frame_rate) if _frame_rate > 0.0 else 0.0)


## Sets the pitch-shift effect to undo the pitch change of speed under audio_pitch_adjust, and to
## 1 without it.
func _apply_pitch_adjust() -> void:
	if audio_pitch_adjust:
		_audio_pitch_effect.pitch_scale = clampf(1.0 / maxf(speed, 0.05), 0.25, 4.0)
	elif _audio_pitch_effect.pitch_scale != 1.0:
		_audio_pitch_effect.pitch_scale = 1.0


## Seeks the audio of a finite source to the presented frame once they drift apart past
## AUDIO_OFFSET_THRESHOLD. Under audio_speed_to_sync a drift past half of it nudges the pitch by 1%.
func _sync_audio_video() -> void:
	if audio_player.stream.get_length() == 0.0:
		return
	var video_time := (_current_frame + 1) / _frame_rate
	var audio_time := audio_player.get_playback_position() + AudioServer.get_time_since_last_mix()
	var offset := audio_time - video_time
	if absf(offset) > AUDIO_OFFSET_THRESHOLD:
		audio_player.seek(video_time)
		audio_player.pitch_scale = speed
	elif audio_speed_to_sync:
		if is_zero_approx(audio_player.pitch_scale - speed):
			if offset > AUDIO_OFFSET_THRESHOLD / 2.0:
				audio_player.pitch_scale = speed * 0.99
			elif offset < -AUDIO_OFFSET_THRESHOLD / 2.0:
				audio_player.pitch_scale = speed * 1.01
		elif not ((audio_player.pitch_scale > speed) != (offset < 0.0)):
			audio_player.pitch_scale = speed
#endregion


#region Inner Classes
## A chapter of the source.
class Chapter:
	## Start in seconds.
	var start: float
	## End in seconds.
	var end: float
	var title: String
#endregion
