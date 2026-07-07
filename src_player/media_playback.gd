class_name MediaPlayback
extends Control
## Threaded media playback for the HMI.
##
## One worker Thread per instance owns the GoZenVideo decoder and pushes decoded
## planes into a mutex-guarded slot; the main thread presents them via
## RenderingServer, drives the AudioStreamPlayer, and emits signals. Live sources
## (RTSP) are latency-first (latest-wins, no active A/V resync); finite files add
## framerate pacing, seek, loop and audio-follow sync. The media catalog is a
## proxy over a one-shot GoZenMetadata probe.
## See docs/superpowers/specs/2026-07-02-threaded-media-playback-design.md

#region Signals
signal media_opened
signal media_closed
signal media_ended            ## Finite sources only: last frame shown.
signal media_disconnected     ## Live: transient connection loss.
signal media_reconnected      ## Live: recovered after a loss.
signal media_error(message: String)
signal playback_started
signal playback_paused
signal frame_changed(frame_nr: int)
#endregion


#region Enums
enum COLOR_PROFILE { AUTO, BT470, BT601, BT709, BT2020, BT2100 }
#endregion


#region Constants
const SHADER_PATH: String = "res://addons/gde_gozen/video_yuva_to_rgba.gdshader"
const AUDIO_OFFSET_THRESHOLD: float = 0.1
const RECONNECT_MAX_ATTEMPTS: int = 5
const RECONNECT_BACKOFF_MSEC: int = 500
const PAUSE_POLL_MSEC: int = 10
#endregion


#region Exported Variables
@export_category("Media")
@export var autoplay: bool = false
@export var loop: bool = false                       ## Finite sources only.
@export_range(0.25, 4.0, 0.05) var speed: float = 1.0 ## Finite sources only.
## Seconds a blocking open/read may stall before it is treated as a dead stream.
## 0 = infinite: close() can still cancel a live (published) decode, but a dead-from-start
## open (metadata/video/audio probe, before the decoder is published) would then stall close()
## indefinitely — keep this > 0 unless you have a reason not to.
@export var network_timeout: float = 5.0

@export_group("Video")
@export var video_enable: bool = true
@export var video_color_profile: COLOR_PROFILE = COLOR_PROFILE.AUTO

@export_group("Audio")
@export var audio_enable: bool = true
@export var audio_stream: int = -1                   ## Audio stream index (-1 = default).
@export var audio_speed_to_sync: bool = false        ## Finite sources only.
@export var audio_pitch_adjust: bool = true

@export_category("Debug")
@export var debug: bool = false
#endregion


#region Public State (read-only)
var video_texture: TextureRect = null
var audio_player: AudioStreamPlayer = null

var path: String:
	get: return _path
var is_open: bool:
	get: return _is_open
var is_ready: bool:
	get: return _is_ready
var is_playing: bool:
	get: return _is_playing
var is_live: bool:
	get: return _is_live

var resolution: Vector2i:
	get: return _resolution
var frame_rate: float:
	get: return _frame_rate
var frame_count: int:
	get: return _frame_count
var current_frame: int:
	get: return _current_frame
var frame_rotation: int:
	get: return _rotation
var has_alpha: bool:
	get: return _has_alpha

var duration: float:
	get: return (_duration_us / 1_000_000.0) if _duration_us > 0 else 0.0
var elapsed: float:
	get: return (_current_frame / _frame_rate) if _frame_rate > 0.0 else 0.0

var video_streams: PackedInt32Array:
	get: return _metadata.get_video_streams() if _metadata != null else PackedInt32Array()
var audio_streams: PackedInt32Array:
	get: return _metadata.get_audio_streams() if _metadata != null else PackedInt32Array()
var subtitle_streams: PackedInt32Array:
	get: return _metadata.get_subtitle_streams() if _metadata != null else PackedInt32Array()
#endregion


#region Private Variables
var _mutex: Mutex = Mutex.new()
var _thread: Thread = Thread.new()

var _path: String = ""
var _video: GoZenVideo = null          # Worker-owned; its reference is published/cleared under _mutex so close() can cancel().
var _audio: AudioStreamFFmpeg = null
var _metadata: GoZenMetadata = null    # Immutable after open(); safe to read anywhere.

var _shader_material: ShaderMaterial = null
var _audio_pitch_effect: AudioEffectPitchShift = null
var _y_tex: ImageTexture = null
var _u_tex: ImageTexture = null
var _v_tex: ImageTexture = null
var _a_tex: ImageTexture = null

# Frame handoff slot (guarded by _mutex).
var _slot_y: Image = null
var _slot_u: Image = null
var _slot_v: Image = null
var _slot_a: Image = null
var _slot_frame: int = 0
var _slot_dirty: bool = false

# Worker command flags (guarded by _mutex).
var _running: bool = false
var _paused: bool = true
var _seek_target: int = -1

# Off-thread audio (re)open state (guarded by _mutex).
var _audio_open_gen: int = 0                    # monotonically bumped per request; latest wins
var _audio_pending: AudioStreamFFmpeg = null    # in-flight instance, so close() can cancel() it
var _audio_task_id: int = -1                    # WorkerThreadPool task id of the in-flight open

# Audio auto-reconnect state (main thread only).
var _audio_reconnecting: bool = false     # a reconnect cycle is active
var _audio_reconnect_attempts: int = 0    # attempts within the current cycle
var _audio_reconnect_next_ms: int = 0     # earliest Time.get_ticks_msec() for the next attempt
var _audio_attempt_inflight: bool = false # an off-thread reopen is in flight (set by us, cleared on result)
var _audio_lost: bool = false             # gave up after the cap (until next open()/successful reopen)

# Main-thread state.
var _is_open: bool = false
var _is_ready: bool = false
var _is_playing: bool = false
var _is_live: bool = true

var _resolution: Vector2i = Vector2i.ZERO
var _frame_rate: float = 0.0
var _frame_count: int = 0
var _current_frame: int = 0
var _rotation: int = 0
var _has_alpha: bool = false
var _duration_us: int = 0
#endregion


#region Built-in Methods
func _enter_tree() -> void:
	_shader_material = ShaderMaterial.new()
	_shader_material.shader = load(SHADER_PATH)

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


func _exit_tree() -> void:
	close()
	if audio_player != null:
		AudioServer.remove_bus(AudioServer.get_bus_index(audio_player.bus))


func _notification(what: int) -> void:
	# Reap the worker thread + in-flight audio pool task on deletion even if the node
	# never entered the tree (so _exit_tree never fired). close() is idempotent, so the
	# in-tree path (_exit_tree already called close()) is a safe no-op here.
	if what == NOTIFICATION_PREDELETE:
		close()


func _process(_delta: float) -> void:
	if not _is_open:
		return

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

	if audio_enable and _audio != null and _is_playing and _is_live and not _audio_lost:
		_poll_audio_health()
#endregion


#region Public Methods
func open(media_path: String = _path) -> void:
	close()

	if media_path.begins_with("uid://"):
		media_path = ResourceUID.get_id_path(ResourceUID.text_to_id(media_path))

	_path = media_path
	_reset_state()
	_is_open = true
	_running = true
	_paused = true
	_thread = Thread.new()
	_thread.start(_open_and_decode)


func play() -> void:
	if not _is_open or _is_playing:
		return
	_is_playing = true
	_mutex.lock()
	_paused = false
	_mutex.unlock()

	if audio_enable and audio_player.stream != null:
		audio_player.pitch_scale = speed
		_apply_pitch_adjust()
		audio_player.set_stream_paused(false)
		if _is_live:
			if not audio_player.playing:
				audio_player.play()
		else:
			audio_player.play((_current_frame / _frame_rate) if _frame_rate > 0.0 else 0.0)

	playback_started.emit()


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


func seek_frame(frame_nr: int) -> void:
	if not _is_open or _is_live:
		return
	var target := clampi(frame_nr, 0, _frame_count)
	_mutex.lock()
	_seek_target = target
	_mutex.unlock()


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

	# Cap to <=1 in-flight open: abort + reap the previous before starting a new one.
	# In the common (non-overlapping) case prev/prev_task are null/-1 -> no main-thread wait.
	if prev != null:
		prev.cancel()
	if prev_task != -1:
		WorkerThreadPool.wait_for_task_completion(prev_task)   # bounded to ~ms by cancel()

	var tid := WorkerThreadPool.add_task(_open_audio_task.bind(stream_index, gen))
	_mutex.lock()
	_audio_task_id = tid
	_mutex.unlock()
	return true


func resync_audio() -> void:
	# Reopen the current audio index off-main: a fresh connection lands live audio at "now"
	# while video keeps running. Meaningful for live; harmless for files (re-lands at position).
	if not _is_open or not audio_enable or _audio == null:
		return
	set_audio_stream(audio_stream)


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
	_mutex.unlock()

	if v != null:
		v.cancel()  # unblock an in-flight next_frame()/open() so wait_to_finish() can't stall

	if _thread.is_started():
		_thread.wait_to_finish()

	# Reap any in-flight off-thread audio open so the pool task never outlives this node.
	# self is alive for the wait; the wait is bounded to ~ms by cancel(). A late
	# _apply_audio_stream already queued via call_deferred self-discards (stale gen / not _is_open).
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
		audio_player.stop()
		audio_player.stream = null
	_audio = null
	_metadata = null
	_current_frame = 0

	media_closed.emit()


func get_stream_title(stream_index: int) -> String:
	return _metadata.get_stream_title(stream_index) if _metadata != null else ""


func get_stream_language(stream_index: int) -> String:
	return _metadata.get_stream_language(stream_index) if _metadata != null else ""


func get_chapter_count() -> int:
	return _metadata.get_chapter_count() if _metadata != null else 0


func get_chapter(chapter_index: int) -> Chapter:
	var c := Chapter.new()
	if _metadata != null:
		c.start = _metadata.get_chapter_start_us(chapter_index) / 1_000_000.0
		c.end = _metadata.get_chapter_end_us(chapter_index) / 1_000_000.0
		c.title = str(_metadata.get_chapter_metadata(chapter_index).get("title", ""))
	return c
#endregion


#region Worker Thread
func _open_and_decode() -> void:
	var meta := GoZenMetadata.new()
	meta.set_network_timeout(network_timeout)
	if meta.open(_path) == OK:
		_metadata = meta

	var has_audio := _metadata != null and not _metadata.get_audio_streams().is_empty()

	if video_enable:
		var v := GoZenVideo.new()
		if debug:
			v.enable_debug()
		v.set_network_timeout(network_timeout)
		if v.open(_path) != OK:
			_notify_error.call_deferred("Cannot open video: %s" % _path)
			return
		_mutex.lock()
		_video = v
		_mutex.unlock()
		if not _video.next_frame(false):
			_notify_error.call_deferred("Cannot decode first frame: %s" % _path)
			return

	if audio_enable and has_audio:
		var a := AudioStreamFFmpeg.new()
		a.set_network_timeout(network_timeout)
		if a.open(_path, audio_stream) == OK:
			_audio = a

	var live := true
	var facts := {"is_live": true}
	if _video != null:
		live = _video.get_frame_count() <= 0
		_store_frame()
		facts = {
			"resolution": _video.get_resolution(),
			"framerate": _video.get_framerate(),
			"rotation": _video.get_rotation(),
			"has_alpha": _video.has_alpha(),
			"full_color": _video.is_full_color_range(),
			"interlaced": _video.get_interlaced(),
			"color_profile": _video.get_color_profile(),
			"frame_count": _video.get_frame_count(),
			"duration_us": _video.get_duration_us(),
			"is_live": live,
		}

	_on_opened.call_deferred(facts)

	if _video == null:
		return  # Audio-only: the extension handles audio; nothing to decode here.

	if live:
		_run_live()
	else:
		_run_finite()

	_video.close()
	_mutex.lock()
	_video = null
	_mutex.unlock()


func _run_live() -> void:
	var attempts := 0
	while _get_running():
		if _get_paused():
			OS.delay_msec(PAUSE_POLL_MSEC)
			continue
		if _video.next_frame(false):
			attempts = 0
			_store_frame()
		else:
			if not _get_running():
				return  # cancelled by close(): don't reconnect/backoff, exit promptly
			attempts += 1
			if attempts > RECONNECT_MAX_ATTEMPTS:
				_notify_error.call_deferred("Live stream lost: %s" % _path)
				return
			_notify_disconnected.call_deferred()
			OS.delay_msec(RECONNECT_BACKOFF_MSEC)
			_video.close()
			if _video.open(_path) == OK and _video.next_frame(false):
				attempts = 0
				_notify_reconnected.call_deferred()
				_store_frame()


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
				_seek_audio.call_deferred(target)
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
					_seek_audio.call_deferred(0)
				last = Time.get_ticks_usec()
				acc = 0
			else:
				_notify_ended.call_deferred()
				_mutex.lock()
				_paused = true
				_mutex.unlock()
			continue

		_store_frame()


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


func _get_running() -> bool:
	_mutex.lock()
	var r := _running
	_mutex.unlock()
	return r


func _get_paused() -> bool:
	_mutex.lock()
	var p := _paused
	_mutex.unlock()
	return p


func _take_seek() -> int:
	_mutex.lock()
	var t := _seek_target
	_seek_target = -1
	_mutex.unlock()
	return t
#endregion


#region Main-thread Notifications (called via call_deferred from the worker)
func _on_opened(facts: Dictionary) -> void:
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

	if _audio != null:
		audio_player.stream = _audio

	_is_ready = true
	media_opened.emit()

	if autoplay:
		play()


func _notify_error(message: String) -> void:
	push_error("MediaPlayback: " + message)
	media_error.emit(message)


func _notify_disconnected() -> void:
	media_disconnected.emit()


func _notify_reconnected() -> void:
	media_reconnected.emit()


func _notify_ended() -> void:
	_is_playing = false
	if audio_enable and audio_player.stream != null:
		audio_player.set_stream_paused(true)
	media_ended.emit()


func _seek_audio(frame_nr: int) -> void:
	if not audio_enable or audio_player.stream == null or _frame_rate <= 0.0:
		return
	if audio_player.stream.get_length() == 0.0:
		return
	audio_player.set_stream_paused(false)
	audio_player.play(frame_nr / _frame_rate)
	audio_player.set_stream_paused(not _is_playing)


func _open_audio_task(stream_index: int, gen: int) -> void:
	# Runs on a WorkerThreadPool thread. The only off-main work is the blocking open().
	var a := AudioStreamFFmpeg.new()
	a.set_network_timeout(network_timeout)   # apply the user's timeout (fixes the old omission)

	_mutex.lock()
	if gen != _audio_open_gen:                # superseded before we even started
		_mutex.unlock()
		_on_audio_open_result.call_deferred(false, gen)
		return
	_audio_pending = a
	_mutex.unlock()

	var err := a.open(_path, stream_index)    # BLOCKING network open, off the main thread

	_mutex.lock()
	if _audio_pending == a:
		_audio_pending = null
	var current := (gen == _audio_open_gen)
	_mutex.unlock()

	if err != OK:
		if current:
			_notify_error.call_deferred("Cannot open audio stream %d" % stream_index)
		_on_audio_open_result.call_deferred(false, gen)
		return
	if not current:
		_on_audio_open_result.call_deferred(false, gen)   # superseded; do not apply
		return
	_apply_audio_stream.call_deferred(a, gen)
	_on_audio_open_result.call_deferred(true, gen)


func _apply_audio_stream(a: AudioStreamFFmpeg, gen: int) -> void:
	# Main thread (deferred). Re-check because a supersede or close() may have raced the open.
	_mutex.lock()
	var current := (gen == _audio_open_gen)
	_mutex.unlock()
	if not current or not _is_open:
		return

	_audio = a
	audio_player.stream = _audio
	if _is_playing:
		audio_player.pitch_scale = speed   # mirror play(): a mid-switch at speed != 1 must not blip
		_apply_pitch_adjust()
		audio_player.set_stream_paused(false)
		if _is_live:
			audio_player.play()
		else:
			audio_player.play((_current_frame / _frame_rate) if _frame_rate > 0.0 else 0.0)


func _poll_audio_health() -> void:
	# Main thread. Detect a latched-dead live audio stream and drive a video-mirrored reconnect cycle.
	if not _audio_reconnecting:
		if _audio.is_stream_healthy():
			return
		_audio_reconnecting = true
		_audio_reconnect_attempts = 0
		_audio_reconnect_next_ms = 0
		_notify_disconnected()   # emits media_disconnected (already on the main thread)

	# A reconnect cycle is active. Fire one attempt only when the previous concluded and backoff elapsed.
	if _audio_attempt_inflight or Time.get_ticks_msec() < _audio_reconnect_next_ms:
		return
	if _audio_reconnect_attempts >= RECONNECT_MAX_ATTEMPTS:
		_audio_reconnecting = false
		_audio_lost = true
		_notify_error("Live audio lost: %s" % _path)
		return
	# Count the attempt + latch inflight ONLY when an open was actually dispatched, so a
	# non-dispatching set_audio_stream can never wedge the cycle with a stuck inflight flag.
	if set_audio_stream(audio_stream):
		_audio_reconnect_attempts += 1
		_audio_attempt_inflight = true


func _on_audio_open_result(ok: bool, gen: int) -> void:
	# Main thread (deferred by _open_audio_task on every conclusion). Paces the reconnect cycle on
	# actual open conclusions, not wall-clock (a failed reopen can take up to network_timeout to fail).
	_mutex.lock()
	var current := (gen == _audio_open_gen)
	_mutex.unlock()
	if not current:
		return   # stale attempt, superseded

	_audio_attempt_inflight = false
	if ok:
		# A fresh healthy _audio was applied by _apply_audio_stream. Clear the whole cycle.
		_audio_lost = false
		_audio_reconnect_attempts = 0
		_audio_reconnect_next_ms = 0
		if _audio_reconnecting:
			_audio_reconnecting = false
			_notify_reconnected()   # emits media_reconnected
	elif _audio_reconnecting:
		_audio_reconnect_next_ms = Time.get_ticks_msec() + RECONNECT_BACKOFF_MSEC
#endregion


#region Helpers (main thread)
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
			return Vector4(1.5748, 0.1873, 0.4681, 1.8556) # bt709 and unknown


func _apply_pitch_adjust() -> void:
	if audio_pitch_adjust:
		_audio_pitch_effect.pitch_scale = clampf(1.0 / maxf(speed, 0.05), 0.25, 4.0)
	elif _audio_pitch_effect.pitch_scale != 1.0:
		_audio_pitch_effect.pitch_scale = 1.0


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
class Chapter:
	var start: float  ## Chapter start in seconds.
	var end: float    ## Chapter end in seconds.
	var title: String
#endregion
