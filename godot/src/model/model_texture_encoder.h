#pragma once

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

namespace godot {

// Turns an authored image into the truecolor TGA a model names: 24 bpp when
// `with_alpha` is false, 32 bpp with the image's alpha otherwise. The source
// is read as the FILE on disk (the PNG under the authoring tree), never the
// imported .ctex, so the bytes do not depend on import settings; the
// encoder is the engine's (formats/tga).
class ModelTextureEncoder : public RefCounted {
	GDCLASS(ModelTextureEncoder, RefCounted)

	String last_error_;

protected:
	static void _bind_methods();

public:
	PackedByteArray encode_image(const Ref<Image> &p_image, bool p_with_alpha);
	PackedByteArray encode_file(const String &p_path, bool p_with_alpha);
	String get_last_error() const { return last_error_; }
};

} // namespace godot
