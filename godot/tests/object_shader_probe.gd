extends Node3D

# Windowed probe (NOT a GUT test, NOT headless): builds one sphere per
# representative object-shader family through NovaObjectShaderCache so Godot
# actually compiles the assembled `_object_baseinc.gdshaderinc` + object.gdshaderinc.
# Any shader compile error prints to stdout; a screenshot lands in user://.
# Run:  "$GODOT_BIN" --path godot res://tests/object_shader_probe.tscn

const CASES := [
	{"tag": "FF_ST_OP",     "emis": 0, "glass": 0},
	{"tag": "FF_MT_OP",     "emis": 0, "glass": 0},
	{"tag": "FF_ST_AB",     "emis": 0, "glass": 0},
	{"tag": "FF_ST_OP_LUM", "emis": 2, "glass": 0},
	{"tag": "VS_DOT3DIFF",  "emis": 0, "glass": 0},
	{"tag": "VS_PHONGT",    "emis": 0, "glass": 0},
	{"tag": "VS_ENVPHONGT", "emis": 0, "glass": 0},
	{"tag": "FFP_GLASS",    "emis": 0, "glass": 1},
	{"tag": "VS_FLAG",      "emis": 0, "glass": 0},
]

var _frames := 0

func _ready() -> void:
	var cam := Camera3D.new()
	cam.position = Vector3(0.0, 0.0, 9.0)
	add_child(cam)

	var cache := NovaObjectShaderCache.get_singleton()
	if cache == null:
		print("PROBE FATAL: NovaObjectShaderCache singleton is null (GDExtension not loaded?)")
		get_tree().quit(1)
		return

	var diffuse := _solid(Color(0.80, 0.70, 0.55, 1.0))
	var flat_normal := _solid(Color(0.5, 0.5, 1.0, 1.0))
	var n := CASES.size()
	var x := -float(n - 1) * 0.5
	for c in CASES:
		var key: int = cache.classify(c["tag"], 0, int(c["emis"]), int(c["glass"]), 128)
		var sh: Shader = cache.get_shader_for_key(key)
		var mat := ShaderMaterial.new()
		mat.shader = sh
		mat.set_shader_parameter("u_diffuse", diffuse)
		mat.set_shader_parameter("u_detail", _solid(Color.WHITE))
		mat.set_shader_parameter("u_normal_map", flat_normal)
		var mi := MeshInstance3D.new()
		var sphere := SphereMesh.new()
		sphere.radius = 0.5
		sphere.height = 1.0
		mi.mesh = sphere
		mi.material_override = mat
		mi.position = Vector3(x * 1.25, 0.0, 0.0)
		add_child(mi)
		x += 1.0
		print("PROBE built tag=", c["tag"], " key=", key)

	print("PROBE all materials built (", n, " families)")

func _process(_delta: float) -> void:
	_frames += 1
	if _frames == 6:
		var img := get_viewport().get_texture().get_image()
		var path := "user://shader_probe.png"
		img.save_png(path)
		print("PROBE screenshot: ", ProjectSettings.globalize_path(path))
	if _frames >= 10:
		print("PROBE done")
		get_tree().quit(0)

func _solid(color: Color) -> ImageTexture:
	var im := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	im.fill(color)
	return ImageTexture.create_from_image(im)
