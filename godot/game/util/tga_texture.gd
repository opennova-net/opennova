class_name TgaTexture
extends RefCounted

## TGA art rides the raw VFS read + Godot's TGA decoder (ResourceRoot's
## load_texture is the PCX path)
## [orig: CTerrainTileData_LoadTGAFromArchive @ 0x520871].


## The decoded texture, else null: no root, an empty name, a name the root
## does not carry, or bytes that do not decode. `force_loose_first` reads with
## ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST (retail's UI-image lookup) instead of
## the session's default policy.
static func load_from_root(root: ResourceRoot, name: String,
		force_loose_first := false) -> Texture2D:
	if root == null or name.is_empty():
		return null
	var bytes := root.read_file(name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST) \
			if force_loose_first else root.read_file(name)
	if bytes.is_empty():
		return null
	var image := Image.new()
	if image.load_tga_from_buffer(bytes) != OK:
		return null
	return ImageTexture.create_from_image(image)
