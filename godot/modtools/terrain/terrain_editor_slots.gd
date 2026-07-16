extends RefCounted

const SLOT_ORDER := [
	"detail_c1",
	"detail_c2",
	"detail_c3",
	"detailmap",
	"detailmapdist",
	"detailmap2",
	"detailmapdist2",
	"charmap",
	"foliagemap",
	"tilestrip",
]

const DETAIL_SLOT_IDS := [
	"detail_c1",
	"detail_c2",
	"detail_c3",
]

const AUX_SLOT_IDS := [
	"detailmap",
	"detailmapdist",
	"detailmap2",
	"detailmapdist2",
]

const MAP_DATA_SLOT_IDS := [
	"charmap",
	"foliagemap",
	"tilestrip",
]

const TEXTURE_FILE_FILTER := "*.tga,*.pcx,*.png"
const TEXTURE_FILE_FILTER_LABEL := "Texture files"
const DEFAULT_EXPORT_IMAGE_SIZE := 512

const SLOT_DEFS := {
	"detail_c1": {
		"label": "Detail A",
		"dialog_title": "Load Detail Texture 1",
		"trn_key": "detailmap_c1",
		"uniform": "u_detail_c1",
		"suffix": "_dc1.tga",
		"bpp": 24,
		"getter": "get_detailmap_c1",
		"setter": "set_detailmap_c1",
		"previewable": true,
		"paint_channel": 0,
	},
	"detail_c2": {
		"label": "Detail B",
		"dialog_title": "Load Detail Texture 2",
		"trn_key": "detailmap_c2",
		"uniform": "u_detail_c2",
		"suffix": "_dc2.tga",
		"bpp": 24,
		"getter": "get_detailmap_c2",
		"setter": "set_detailmap_c2",
		"previewable": true,
		"paint_channel": 1,
	},
	"detail_c3": {
		"label": "Detail C",
		"dialog_title": "Load Detail Texture 3",
		"trn_key": "detailmap_c3",
		"uniform": "u_detail_c3",
		"suffix": "_dc3.tga",
		"bpp": 24,
		"getter": "get_detailmap_c3",
		"setter": "set_detailmap_c3",
		"previewable": true,
		"paint_channel": 2,
	},
	"detailmap": {
		"label": "Detail coefficient source",
		"dialog_title": "Load Detail Coefficient Source",
		"tooltip": "Authored detail source. Retail generates the shader coefficient map from this texture's blue channel at scale 1/32.",
		"trn_key": "detailmap",
		"uniform": "u_detailmap",
		"suffix": "_dm.tga",
		"bpp": 32,
		"getter": "get_detailmap",
		"setter": "set_detailmap",
		"previewable": true,
	},
	"detailmapdist": {
		"label": "Far detail mip target",
		"dialog_title": "Load Far Detail Mip Target",
		"tooltip": "Far RGB target blended into Detail A/B/C's custom mip chains. It is not a camera-distance normal map.",
		"trn_key": "detailmapdist",
		"uniform": "",
		"suffix": "_dmd.tga",
		"bpp": 24,
		"getter": "get_detailmapdist",
		"setter": "set_detailmapdist",
		"previewable": true,
	},
	"detailmap2": {
		"label": "Detail layer 2",
		"dialog_title": "Load Detail Layer 2",
		"tooltip": "Second detail texture blended over the splat layers at its own density (Detail density 2).",
		"trn_key": "detailmap2",
		"uniform": "",
		"suffix": "_dm2.tga",
		"bpp": 24,
		"getter": "get_detailmap2",
		"setter": "set_detailmap2",
		"previewable": true,
	},
	"detailmapdist2": {
		"label": "Detail layer 2 distance target",
		"dialog_title": "Load Detail Layer 2 Distance Target",
		"tooltip": "Far color the second detail layer fades toward with distance.",
		"trn_key": "detailmapdist2",
		"uniform": "",
		"suffix": "_dmd2.tga",
		"bpp": 24,
		"getter": "get_detailmapdist2",
		"setter": "set_detailmapdist2",
		"previewable": true,
	},
	"charmap": {
		"label": "Surface types",
		"dialog_title": "Load Surface Types",
		"trn_key": "charmap",
		"uniform": "u_charmap",
		"suffix": "_m.pcx",
		"bpp": 24,
		"getter": "get_charmap_tex",
		"setter": "set_charmap_tex",
		"previewable": true,
		"format": "pcx_paletted",
	},
	"foliagemap": {
		"label": "Foliage placement",
		"dialog_title": "Load Foliage Placement",
		"trn_key": "foliagemap",
		"uniform": "u_foliagemap",
		"suffix": "_f.pcx",
		"bpp": 24,
		"getter": "get_foliagemap_tex",
		"setter": "set_foliagemap_tex",
		"previewable": true,
		"format": "pcx_paletted",
	},
	"tilestrip": {
		"label": "Tile atlas",
		"dialog_title": "Load Tile Atlas",
		"trn_key": "tilestrip",
		"uniform": "u_tilestrip",
		"suffix": "_t.tga",
		"bpp": 24,
		"getter": "get_tilestrip_tex",
		"setter": "set_tilestrip_tex",
		"previewable": true,
	},
}

const DETAIL_PLACEHOLDER_COLORS := [
	Color(0.3, 0.5, 0.2, 1.0),
	Color(0.6, 0.5, 0.3, 1.0),
	Color(0.5, 0.5, 0.55, 1.0),
]

const FAR_DETAIL_PLACEHOLDER_COLOR := Color(0.5, 0.5, 0.5, 1.0)


static func get_slot_ids() -> Array:
	return SLOT_ORDER.duplicate()


static func get_detail_slot_ids() -> Array:
	return DETAIL_SLOT_IDS.duplicate()


static func get_aux_slot_ids() -> Array:
	return AUX_SLOT_IDS.duplicate()


static func get_map_data_slot_ids() -> Array:
	return MAP_DATA_SLOT_IDS.duplicate()


static func get_slot(slot_id: String) -> Dictionary:
	return SLOT_DEFS.get(slot_id, {})


static func get_slot_label(slot_id: String) -> String:
	return String(get_slot(slot_id).get("label", slot_id))


static func get_slot_dialog_title(slot_id: String) -> String:
	return String(get_slot(slot_id).get("dialog_title", "Load Texture"))


static func get_texture_file_filters() -> PackedStringArray:
	return PackedStringArray(["%s ; %s" % [TEXTURE_FILE_FILTER, TEXTURE_FILE_FILTER_LABEL]])


static func get_detail_slot_id(channel: int) -> String:
	if channel < 0 or channel >= DETAIL_SLOT_IDS.size():
		return ""
	return String(DETAIL_SLOT_IDS[channel])


static func get_export_filename(slot_id: String, terrain_name: String) -> String:
	return terrain_name + String(get_slot(slot_id).get("suffix", ""))


static func is_previewable(slot_id: String) -> bool:
	return bool(get_slot(slot_id).get("previewable", false))


static func load_image_from_file(path: String) -> Image:
	var bytes := FileAccess.get_file_as_bytes(path)
	if bytes.is_empty():
		return null
	var image := Image.new()
	var err := OK
	match path.get_extension().to_lower():
		"png":
			err = image.load_png_from_buffer(bytes)
		"tga":
			err = image.load_tga_from_buffer(bytes)
		_:
			err = image.load(path)
	if err != OK:
		return null
	return normalize_image(image)


static func normalize_image(image: Image) -> Image:
	if image == null:
		return null
	if image.is_compressed():
		image.decompress()
	if image.get_format() != Image.FORMAT_RGBA8:
		image.convert(Image.FORMAT_RGBA8)
	image.generate_mipmaps()
	return image


static func create_texture(image: Image) -> Texture2D:
	if image == null:
		return null
	return ImageTexture.create_from_image(image)


static func texture_to_image(texture: Texture2D) -> Image:
	if texture == null:
		return null
	return normalize_image(texture.get_image())


static func get_slot_texture(data, slot_id: String) -> Texture2D:
	if data == null:
		return null
	var slot: Dictionary = get_slot(slot_id)
	if slot.is_empty():
		return null
	return data.call(String(slot["getter"]))


static func apply_slot_texture(material: ShaderMaterial, data, slot_id: String, texture: Texture2D) -> void:
	if material == null:
		return
	var slot: Dictionary = get_slot(slot_id)
	if slot.is_empty():
		return
	var shader_uniform := String(slot.get("uniform", ""))
	if not shader_uniform.is_empty():
		material.set_shader_parameter(shader_uniform, texture)
	if data != null:
		data.call(String(slot["setter"]), texture)


static func apply_slot_image(material: ShaderMaterial, data, slot_id: String, image: Image) -> Texture2D:
	var texture := create_texture(image)
	apply_slot_texture(material, data, slot_id, texture)
	return texture


static func apply_slots_from_data(material: ShaderMaterial, data) -> void:
	for slot_id in SLOT_ORDER:
		apply_slot_texture(material, data, slot_id, get_slot_texture(data, slot_id))


static func apply_default_slots(material: ShaderMaterial, data, sync_data: bool = true) -> void:
	for index in range(DETAIL_SLOT_IDS.size()):
		var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
		image.fill(DETAIL_PLACEHOLDER_COLORS[index])
		var slot_id := String(DETAIL_SLOT_IDS[index])
		var texture := create_texture(image)
		material.set_shader_parameter(String(get_slot(slot_id)["uniform"]), texture)
		if sync_data and data != null:
			data.call(String(get_slot(slot_id)["setter"]), texture)
	var normal_image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	normal_image.fill(FAR_DETAIL_PLACEHOLDER_COLOR)
	var normal_texture := create_texture(normal_image)
	apply_slot_texture(material, data if sync_data else null, "detailmapdist", normal_texture)
	apply_slot_texture(material, data if sync_data else null, "detailmap", null)
	apply_slot_texture(material, data if sync_data else null, "detailmap2", null)
	apply_slot_texture(material, data if sync_data else null, "detailmapdist2", null)
	for map_slot_id in MAP_DATA_SLOT_IDS:
		var map_slot: Dictionary = get_slot(String(map_slot_id))
		material.set_shader_parameter(String(map_slot["uniform"]), null)
		if sync_data and data != null:
			data.call(String(map_slot["setter"]), null)


static func apply_default_slot(material: ShaderMaterial, data, slot_id: String, sync_data: bool = true) -> void:
	if slot_id in DETAIL_SLOT_IDS:
		var index := DETAIL_SLOT_IDS.find(slot_id)
		var image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
		image.fill(DETAIL_PLACEHOLDER_COLORS[index])
		apply_slot_texture(material, data, slot_id, create_texture(image))
		return
	if slot_id == "detailmapdist":
		var normal_image := Image.create(4, 4, false, Image.FORMAT_RGBA8)
		normal_image.fill(FAR_DETAIL_PLACEHOLDER_COLOR)
		apply_slot_texture(material, data, slot_id, create_texture(normal_image))
		return
	if slot_id == "detailmap" or slot_id == "detailmap2" or slot_id == "detailmapdist2":
		apply_slot_texture(material, data, slot_id, null)
		return
	if slot_id in MAP_DATA_SLOT_IDS:
		apply_slot_texture(material, data, slot_id, null)
		return


static func uses_placeholder_default(slot_id: String) -> bool:
	return slot_id in DETAIL_SLOT_IDS or slot_id == "detailmapdist"


static func create_export_placeholder_image() -> Image:
	var image := Image.create(DEFAULT_EXPORT_IMAGE_SIZE, DEFAULT_EXPORT_IMAGE_SIZE, false, Image.FORMAT_RGBA8)
	image.fill(FAR_DETAIL_PLACEHOLDER_COLOR)
	return image
