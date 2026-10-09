#include <editor/documents/texture_budget.h>

#include <cstdio>

namespace opennova::editor {

namespace {

using renderer::DeviceTexture;
using renderer::DeviceTextureFormat;

io::JsonValue device_json(const DeviceTexture &texture, int level) {
	using io::JsonValue;
	JsonValue out = JsonValue::make_object();
	if (level >= 0) out.set("level", JsonValue::make_number(level));
	out.set("width", JsonValue::make_number(double(texture.width)));
	out.set("height", JsonValue::make_number(double(texture.height)));
	out.set("format", JsonValue::make_string(renderer::device_texture_format_name(texture.format)));
	out.set("levels", JsonValue::make_number(double(texture.levels)));
	out.set("bytes", JsonValue::make_number(double(texture.bytes)));
	out.set("stat_bytes", JsonValue::make_number(double(texture.stat_bytes)));
	out.set("halvings", JsonValue::make_number(double(texture.halvings)));
	out.set("whole", JsonValue::make_bool(texture.whole));
	out.set("dxt5_as_dxt1", JsonValue::make_bool(texture.dxt5_as_dxt1));
	return out;
}

} // namespace

bool texture_role_budget_loader(renderer::TextureRoleId role, renderer::TextureLoader &out) {
	switch (role) {
	case renderer::TextureRoleId::ModelDiffuse:
	case renderer::TextureRoleId::ModelDetail:
	case renderer::TextureRoleId::ModelFlipFrame: out = renderer::TextureLoader::Stage; return true;
	case renderer::TextureRoleId::ModelPlain: out = renderer::TextureLoader::Plain; return true;
	case renderer::TextureRoleId::ModelNormalMap:
	case renderer::TextureRoleId::ModelHeightNormal: out = renderer::TextureLoader::Normal; return true;
	default: return false;
	}
}

// Each detail level's device texture as the row's loader makes it (renderer::model_row_device_texture: the
// stage loader alone reads a DDS through D3DX).
TextureBudget texture_budget(const TextureHeader &header, const std::string &file, renderer::TextureLoader loader, uint8_t slot) {
	TextureBudget out;
	if (!header.read || header.width == 0 || header.height == 0) return out;
	out.known = true;
	out.loader = loader;
	out.slot = slot;
	out.file = file;
	const bool dds = header.reader == TextureReader::Dds;
	renderer::DdsSource source;
	source.width = header.width;
	source.height = header.height;
	if (dds) {
		source.levels = header.dds_levels;
		source.format = renderer::dds_device_format(header.dds_format);
		source.bits = header.dds_bits ? header.dds_bits : 32;
		source.dxt5_opaque = header.dds_dxt5_opaque;
	}
	for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level)
		out.detail[level] = renderer::model_row_device_texture(loader, slot, level, source, dds);
	if (loader == renderer::TextureLoader::Stage && header.reader != TextureReader::Dds) {
		renderer::DdsSource made;
		made.width = header.width;
		made.height = header.height;
		made.levels = renderer::full_chain_levels(header.width, header.height);
		made.format = header.alpha ? DeviceTextureFormat::Dxt5 : DeviceTextureFormat::Dxt1;
		out.offers_dds = true;
		out.as_dds = renderer::dds_device_texture(made, 0);
	}
	return out;
}

std::string texture_bytes_words(uint64_t bytes) {
	char text[48];
	if (bytes >= uint64_t(1024) * 1024) std::snprintf(text, sizeof(text), "%.1f MB", double(bytes) / (1024.0 * 1024.0));
	else if (bytes >= 1024) std::snprintf(text, sizeof(text), "%.0f KB", double(bytes) / 1024.0);
	else std::snprintf(text, sizeof(text), "%llu bytes", static_cast<unsigned long long>(bytes));
	return text;
}

std::string device_texture_words(const DeviceTexture &texture) {
	std::string format = renderer::device_texture_format_name(texture.format);
	if (texture.format == DeviceTextureFormat::A8R8G8B8) format += " (uncompressed)";
	if (texture.dxt5_as_dxt1) format += " (a DXT5 the game stores as DXT1, every block opaque)";
	return std::to_string(texture.width) + " x " + std::to_string(texture.height) + ", " + format + ", " +
	       std::to_string(texture.levels) + (texture.levels == 1 ? " level" : " levels") + ": " + texture_bytes_words(texture.bytes);
}

std::string texture_budget_words(const TextureBudget &budget) {
	if (!budget.known) return "";
	const DeviceTexture &full = budget.full();
	std::string out = texture_bytes_words(full.bytes) + " in the game (" + device_texture_words(full) + ")";
	if (budget.offers_dds)
		out += "; " + texture_bytes_words(budget.as_dds.bytes) + " as a " +
		       renderer::device_texture_format_name(budget.as_dds.format) + " .dds";
	return out;
}

io::JsonValue texture_budget_json(const TextureBudget &budget) {
	using io::JsonValue;
	JsonValue out = JsonValue::make_object();
	out.set("loader", JsonValue::make_string(texture_loader_token(budget.loader)));
	out.set("slot", JsonValue::make_number(budget.slot));
	out.set("file", JsonValue::make_string(budget.file));
	JsonValue detail = JsonValue::make_array();
	for (int level = 0; level < renderer::kObjectTexDetailLevels; ++level) detail.push(device_json(budget.detail[level], level));
	out.set("detail", std::move(detail));
	out.set("as_dds", budget.offers_dds ? device_json(budget.as_dds, -1) : JsonValue::make_null());
	out.set("words", JsonValue::make_string(texture_budget_words(budget)));
	return out;
}

} // namespace opennova::editor
