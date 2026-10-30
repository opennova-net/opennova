extends GutTest

## Runtime audio device tests: LwfData's model writes, SoundBank's name index and MenuAudio's
## bank gate, and the one stream the engine mixer plays through (AudioOut). The sound logic --
## the one-shot plans, the emitter mix, the wave decode, the mixer's channels and arithmetic --
## is the engine's, pinned by ctest (audio_oneshot_play, ambient_mixer, wav_pcm, audio_mixer).
## Self-contained (no real game data required).



# A REAL ResourceRoot over a per-test temp dir (ADR 0034 typed seam): the
# in-memory wav bytes land as files, and the bank reads them through the same
# native VFS the runtime uses.
var _root_dirs: Array[String] = []


func after_each() -> void:
	for dir in _root_dirs:
		TestFs.remove_dir_recursive(dir)
	_root_dirs.clear()


func _real_root(files: Dictionary) -> ResourceRoot:
	var dir := OS.get_temp_dir().replace("\\", "/") + 			"/opennova_sound_rt_%d_%d" % [Time.get_ticks_usec(), _root_dirs.size()]
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	_root_dirs.append(dir)
	for name in files.keys():
		var file := FileAccess.open(dir.path_join(String(name)), FileAccess.WRITE)
		assert_not_null(file)
		if file != null:
			file.store_buffer(files[name])
			file.close()
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	return root


func _profile_with_set(set_name: String, wav: String) -> LwfData:
	var d := LwfData.new()
	d.create_empty()
	var si := d.add_set()
	d.set_set_field(si, "name", set_name)
	var li := d.add_layer(si)
	d.set_layer_field(si, li, "internal", true)
	d.set_layer_field(si, li, "external", true)
	var mi := d.add_member(si, li)
	d.set_member_field(si, li, mi, "wav_path", wav)
	return d


# A bank of one set, one layer and three members (the first two naming one member
# record, all three one single), laid out as formats/lwf's writer lays one out and its
# stale bytes zero: header 28, single 52, multi header 20, set 80, layer 48, two
# member records 28 each, one 256-byte path slot.
func _shared_record_bank() -> PackedByteArray:
	var b := PackedByteArray()
	b.resize(540)
	# Header: size, 'LWF1', one single, no trigger, the multi header, the string pool.
	for pair in [[0, 28], [4, 0x4C574631], [8, 1], [12, 0], [16, 80], [20, 284]]:
		b.encode_u32(pair[0], pair[1])
	b = _with_text(b, 28, "A")
	b.encode_u32(28 + 48, 284) # the single's path slot
	for pair in [[80, 20], [84, 1], [88, 100]]:
		b.encode_u32(pair[0], pair[1])
	b.encode_u32(100, 80)
	b = _with_text(b, 104, "ONE")
	b.encode_u32(128, 0xFFFF) # pitch base
	b.encode_u32(136, 1) # one layer
	b.encode_u32(140, 180) # at the layer table
	b.encode_u16(180, 3) # three members
	b.encode_u16(184, 100) # falloff
	b.encode_u32(188, 0x0E) # random, internal, external
	for pair in [[196, 228], [200, 228], [204, 256]]:
		b.encode_u32(pair[0], pair[1])
	for record in [[228, 200], [256, 150]]:
		b.encode_u32(record[0], 0) # single 0
		b.encode_u32(record[0] + 4, 0x10000) # unity pitch
		b.encode_u32(record[0] + 12, record[1]) # volume
		b.encode_u32(record[0] + 16, 255) # clamp
	b = _with_text(b, 284, "a.wav")
	return b


# `b` with `text`'s ASCII bytes at `at`.
func _with_text(b: PackedByteArray, at: int, text: String) -> PackedByteArray:
	var ascii := text.to_ascii_buffer()
	for i in ascii.size():
		b[at + i] = ascii[i]
	return b


func _member_values(lwf: LwfData, key: String) -> Array:
	var out := []
	var layer := ((lwf.get_set(0).get("layers", []) as Array)[0]) as Dictionary
	for member in layer.get("members", []):
		out.append((member as Dictionary).get(key))
	return out


# LwfData writes its model (opennova::lwf::File) alone, edited or not: a bank whose stale
# bytes are zero comes back byte for byte from its fields, and an edit of a member whose
# record or single another member shares copies it for the edited member alone.
func test_lwf_data_writes_its_model_and_copies_a_shared_record() -> void:
	var bank := _shared_record_bank()
	var lwf := LwfData.new()
	assert_true(lwf.load_bytes(bank), "the bank reads: %s" % lwf.get_last_error())
	assert_eq(_member_values(lwf, "volume"), [200, 200, 150], "two members name one record")
	assert_eq(lwf.to_bytes(), bank, "the bank from its fields, byte for byte")
	lwf.set_member_field(0, 0, 0, "volume", 50)
	assert_eq(_member_values(lwf, "volume"), [50, 200, 150], "the shared record copied for the edited member")
	lwf.set_member_field(0, 0, 2, "name", "B")
	assert_eq(_member_values(lwf, "name"), ["A", "A", "B"], "the shared single copied for the edited member")
	assert_eq(_member_values(lwf, "wav_path"), ["a.wav", "a.wav", "a.wav"], "the copy keeps the single's file")
	var again := LwfData.new()
	assert_true(again.load_bytes(lwf.to_bytes()), "the edited bank reads back")
	assert_eq(again.get_set(0), lwf.get_set(0), "the edited bank reads back as its model")
	lwf.set_layer_field(0, 0, "selection_mode", LwfData.SELECTION_SEQUENTIAL)
	assert_eq(int(((lwf.get_set(0).get("layers", []) as Array)[0] as Dictionary).get("selection_mode", -1)),
			LwfData.SELECTION_SEQUENTIAL, "a layer field set in place")
	assert_true(bool(((lwf.get_set(0).get("layers", []) as Array)[0] as Dictionary).get("internal", false)),
			"its other flags kept")


# A <SOUND> whose bank does not open, or that names none, plays nothing: the
# game's parse leaves the row no bank and its play site consults no other
# (engine/runtime/menu/menu_sound.h carries the witness).
func test_menu_sound_without_its_bank_plays_nothing() -> void:
	var samples := PackedByteArray()
	samples.resize(32)
	var root := _real_root({
		"click.wav": _build_wav(samples, 1, 22050, 16),
		"menu.lwf": _profile_with_set("CLICK_SELECT", "click.wav").to_bytes(),
	})
	var audio := MenuAudio.new()
	add_child_autofree(audio)
	audio.set_resource_root(root)

	assert_false(audio.play_widget_sound("CLICK_SELECT", "missing.lwf"),
			"a bank that does not open plays nothing, menu.lwf in the root or not")
	assert_false(audio.play_widget_sound("CLICK_SELECT", ""),
			"a file-less element plays nothing")
	assert_true(audio.play_widget_sound("CLICK_SELECT", "menu.lwf"),
			"the bank the element names plays")


func test_bank_indexing_case_insensitive() -> void:
	var bank = SoundBank.create(null)
	bank.add_bank(_profile_with_set("Z00AMB1", "Z00aR100.wav"))
	assert_true(bank.has_set("Z00AMB1"), "exact name resolves")
	assert_true(bank.has_set("z00amb1"), "lookup is case-insensitive")
	assert_false(bank.has_set("NOPE"), "unknown set does not resolve")
	assert_true("z00amb1" in bank.get_set_names())


# The engine mixes every sound into one stream, which the device node pumps into one
# AudioStreamGenerator on the Master bus (ADR 0077 d4): a menu play attaches it and it mixes.
func test_a_play_attaches_the_one_stream_device_and_it_mixes() -> void:
	var samples := PackedByteArray()
	samples.resize(4410 * 2)
	var root := _real_root({
		"click.wav": _build_wav(samples, 1, 22050, 16),
		"menu.lwf": _profile_with_set("CLICK_SELECT", "click.wav").to_bytes(),
	})
	var audio := MenuAudio.new()
	add_child_autofree(audio)
	audio.set_resource_root(root)
	assert_true(audio.play_widget_sound("CLICK_SELECT", "menu.lwf"), "the trigger opens a channel")
	await get_tree().process_frame
	var device := get_tree().root.get_node_or_null("AudioOut")
	assert_not_null(device, "the device node sits under the root")
	if device != null:
		var stream := device.get_node_or_null("Stream") as AudioStreamPlayer
		assert_not_null(stream, "its one player")
		if stream != null:
			assert_true(stream.stream is AudioStreamGenerator, "plays a generator")
			assert_eq(stream.bus, &"Master", "on the Master bus")
	var before := AudioOut.frames_mixed()
	await get_tree().process_frame
	await get_tree().process_frame
	assert_true(AudioOut.frames_mixed() >= before, "the device keeps the stream filled")


func _build_wav(samples: PackedByteArray, channels: int, rate: int, bits: int) -> PackedByteArray:
	var data_size := samples.size()
	var byte_rate := rate * channels * (bits / 8)
	var block_align := channels * (bits / 8)
	var buf := StreamPeerBuffer.new()
	buf.big_endian = false
	buf.put_data("RIFF".to_ascii_buffer())
	buf.put_u32(36 + data_size)
	buf.put_data("WAVE".to_ascii_buffer())
	buf.put_data("fmt ".to_ascii_buffer())
	buf.put_u32(16)
	buf.put_u16(1)            # PCM
	buf.put_u16(channels)
	buf.put_u32(rate)
	buf.put_u32(byte_rate)
	buf.put_u16(block_align)
	buf.put_u16(bits)
	buf.put_data("data".to_ascii_buffer())
	buf.put_u32(data_size)
	buf.put_data(samples)
	return buf.data_array
