#include <editor/preview/texture_viewport.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <sstream>

#include <runtime/renderer/texture_dxt.h>
#include <formats/trn/trn_io.h>
#include <runtime/renderer/texture_load_rules.h>

#include <editor/assets/asset_registry.h>
#include <editor/documents/texture_document.h>
#include <editor/import/texture_import.h>
#include <editor/preview/texture_canvas.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
#include <editor/session/texture_import_state.h>
#include <editor/session/texture_use_index.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

// The size its device draws at where no canvas sizes it (a headless Shell's): the model's.
constexpr ViewportState kHeadlessSize{800, 600};
constexpr float kLeastScale = kTextureZoomSteps[0];
constexpr float kMostScale = kTextureZoomSteps[std::size(kTextureZoomSteps) - 1];

constexpr const char *kChannelTokens[] = {"rgb", "red", "green", "blue", "alpha", "rgba", "normals"};

JsonValue camera_to_json(const TextureCamera &camera) {
	JsonValue out = JsonValue::make_object();
	out.set("fit", JsonValue::make_bool(camera.fit));
	out.set("scale", json_number(camera.scale));
	out.set("x", json_number(camera.x));
	out.set("y", json_number(camera.y));
	return out;
}

JsonValue options_to_json(const TextureViewportOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("channels", json_string(texture_channels_token(options.channels)));
	out.set("level", json_number(options.level));
	out.set("as_used", json_number(options.as_used));
	out.set("detail", json_number(options.detail));
	out.set("light", json_number(options.light));
	out.set("compare", json_string(texture_compare_view_token(options.compare)));
	out.set("split", json_number(options.split));
	return out;
}

// A SetViewport's options: {channels, level, as_used, detail, light, compare, split}, each optional.
bool read_options(const JsonValue &json, TextureViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object {channels, level, as_used, detail, light, compare, split}.";
		return false;
	}
	TextureViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "channels") {
			if (!member.value.is_string() || !texture_channels_from_token(member.value.string, options.channels)) {
				error = "options.channels is one of rgb, red, green, blue, alpha, rgba, normals.";
				return false;
			}
		} else if (member.key == "level") {
			if (!member.value.is_number() || member.value.number < 0 || member.value.number > 31 ||
			    std::floor(member.value.number) != member.value.number) {
				error = "options.level is a whole number from 0 (the texture) to 31.";
				return false;
			}
			options.level = int(member.value.number);
		} else if (member.key == "as_used") {
			if (!member.value.is_number() || member.value.number < -1 || member.value.number > 65535 ||
			    std::floor(member.value.number) != member.value.number) {
				error = "options.as_used is -1 (the file itself) or a use's index among the texture's uses.";
				return false;
			}
			options.as_used = int(member.value.number);
		} else if (member.key == "detail") {
			if (!member.value.is_number() || member.value.number < -1 || member.value.number > 3 ||
			    std::floor(member.value.number) != member.value.number) {
				error = "options.detail is -1 (the texture as stored) or an object texture detail level, 0 the lowest to 3 full.";
				return false;
			}
			options.detail = int(member.value.number);
		} else if (member.key == "light") {
			if (!member.value.is_number() || !std::isfinite(member.value.number)) {
				error = "options.light is the light's direction in degrees round the picture.";
				return false;
			}
			options.light = float(std::fmod(std::fmod(member.value.number, 360.0) + 360.0, 360.0));
		} else if (member.key == "compare") {
			if (!member.value.is_string() || !texture_compare_view_from_token(member.value.string, options.compare)) {
				error = "options.compare is one of off, split, dds, difference.";
				return false;
			}
		} else if (member.key == "split") {
			if (!member.value.is_number() || !(member.value.number >= 0.0 && member.value.number <= 1.0)) {
				error = "options.split is a fraction of the texture's width, from 0 to 1.";
				return false;
			}
			options.split = float(member.value.number);
		} else {
			error = "Unknown options member \"" + member.key + "\" (it takes channels, level, as_used, detail, light, compare, split).";
			return false;
		}
	}
	held = options;
	return true;
}

// A SetViewport's camera: {fit, scale, x, y}, each optional; a scale or a middle given makes the
// camera no longer fitted unless fit says so.
bool read_camera(const JsonValue &json, TextureCamera &held, std::string &error) {
	if (!json.is_object()) {
		error = "camera is an object {fit, scale, x, y}.";
		return false;
	}
	TextureCamera camera = held;
	bool fit_given = false;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "fit") {
			if (!member.value.is_bool()) {
				error = "camera.fit is true or false.";
				return false;
			}
			camera.fit = member.value.boolean;
			fit_given = true;
		} else if (member.key == "scale" || member.key == "x" || member.key == "y") {
			if (!member.value.is_number() || !std::isfinite(member.value.number)) {
				error = "camera." + member.key + " is a number.";
				return false;
			}
			const float value = float(member.value.number);
			if (member.key == "scale") {
				if (!(value >= kLeastScale && value <= kMostScale)) {
					error = "camera.scale is from 1/32 to 64 (picture pixels a texel).";
					return false;
				}
				camera.scale = value;
			} else {
				(member.key == "x" ? camera.x : camera.y) = value;
			}
			if (!fit_given) camera.fit = false;
		} else {
			error = "Unknown camera member \"" + member.key + "\" (it takes fit, scale, x, y).";
			return false;
		}
	}
	held = camera;
	return true;
}

} // namespace

TextureShownUse texture_shown_use(const TextureUse &use, int index) {
	TextureShownUse out;
	out.index = index;
	out.words = use.words;
	out.role = use.role;
	out.transform = use.load.transform;
	// A material that cuts out by alpha keeps a texel above its reference, or at or below it inverted
	// [orig: CRenderBatchQueue_FlushBatches @ 0x5DA3A9..0x5DA401].
	// The row the material's technique cuts out by (TextureRowContext: a diffuse's, or the normal map's).
	if ((use.role == TextureRoleId::ModelDiffuse || use.role == TextureRoleId::ModelFlipFrame ||
	     use.role == TextureRoleId::ModelNormalMap) &&
	    use.context.alpha_test()) {
		out.cutout = use.context.alpha_ref;
		out.inverted = use.context.alpha_test_inverted();
	}
	// A tile atlas is cut in 64-texel cells [orig: Terrain_LoadTileSetAtlas @ 0x604B7C].
	if (use.role == TextureRoleId::TerrainTileAtlas) out.cells = 64;
	// What its alpha is to the game: a model row's by its material's technique, else its role's.
	TextureBudgetLoader loader = TextureBudgetLoader::Stage;
	if (use.context.material >= 0 && texture_role_budget_loader(use.role, loader)) {
		out.model_row = true;
		out.alpha = texture_row_alpha_meaning(use.context.shader, use.context.material_flags, use.context.type, use.context.slot,
		                                      use.name_written);
		out.alpha_words = texture_alpha_meaning_words(out.alpha, use.context.shader, use.context.alpha_ref,
		                                              use.context.alpha_test_inverted());
	} else if (use.known()) {
		out.alpha_words = texture_role_row(use.role).alpha;
	}
	if (use.role == TextureRoleId::ParticleGraphic) out.blend_mode = use.context.blend_mode;
	return out;
}

std::vector<TextureLevel> texture_game_chain(const TextureLevel &first, uint32_t levels) {
	std::vector<TextureLevel> out{first};
	if (first.width == 0 || first.height == 0 || first.rgba.size() != size_t(first.width) * first.height * 4) return out;
	const uint32_t count = levels ? levels : renderer::full_chain_levels(first.width, first.height);
	std::vector<renderer::DxtColor> colours = renderer::decode_rgba8(first.rgba.data(), first.width, first.height);
	uint32_t w = first.width, h = first.height;
	while (out.size() < count && (w > 1 || h > 1)) {
		colours = renderer::box_filter_half(colours, w, h);
		w = std::max(1u, w / 2);
		h = std::max(1u, h / 2);
		TextureLevel level;
		level.width = w;
		level.height = h;
		level.rgba = renderer::encode_rgba8(colours);
		out.push_back(std::move(level));
	}
	return out;
}

TextureLevel texture_halved(const TextureLevel &level, uint32_t halvings) {
	TextureLevel out = level;
	for (uint32_t i = 0; i < halvings && out.width > 1 && out.height > 1; ++i) {
		// One halving: the cap at half the larger side halves both once.
		renderer::halve_rgba_to_cap(out.rgba, out.width, out.height, std::max(out.width, out.height) / 2);
		out.width = std::max(1u, out.width);
		out.height = std::max(1u, out.height);
	}
	return out;
}

TextureLevel texture_lit_normals(const TextureLevel &level, float light) {
	TextureLevel out = level;
	const float radians = light * 3.14159265f / 180.0f;
	// The light: round the picture by `light`, across and up (the picture's down is the texture's +green), and
	// above it.
	float lx = std::cos(radians), ly = -std::sin(radians), lz = 1.0f;
	const float length = std::sqrt(lx * lx + ly * ly + lz * lz);
	lx /= length;
	ly /= length;
	lz /= length;
	for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
		float nx = out.rgba[i] / 127.5f - 1.0f, ny = out.rgba[i + 1] / 127.5f - 1.0f, nz = out.rgba[i + 2] / 127.5f - 1.0f;
		const float n = std::sqrt(nx * nx + ny * ny + nz * nz);
		const float lit = n > 0.0f ? std::max(0.0f, (nx * lx + ny * ly + nz * lz) / n) : 0.0f;
		const uint8_t grey = uint8_t(std::lround(std::min(1.0f, lit) * 255.0f));
		out.rgba[i] = out.rgba[i + 1] = out.rgba[i + 2] = grey;
		out.rgba[i + 3] = 255;
	}
	return out;
}

std::shared_ptr<const TextureImage> texture_as_used(const std::shared_ptr<const TextureImage> &image,
                                                    const TextureShownUse &use) {
	if (!image) return image;
	std::shared_ptr<const TextureImage> made =
	        use.transform == TextureLoadTransform::None ? image : apply_load_transform(*image, use.transform);
	made = texture_role_texels(made, use.role, use.blend_mode);
	if (made && use.model_row && use.cutout < 0 && !texture_alpha_is_transparency(use.alpha) &&
	    use.alpha != TextureAlphaMeaning::Height) {
		// The game draws it opaque: whatever its alpha holds is no transparency here.
		auto opaque = std::make_shared<TextureImage>(*made);
		for (TextureLevel &level : opaque->levels)
			for (size_t i = 3; i < level.rgba.size(); i += 4) level.rgba[i] = 255;
		opaque->alpha = TextureAlpha::None;
		return opaque;
	}
	if (use.cutout < 0 || !made) return made;
	auto out = std::make_shared<TextureImage>(*made);
	for (TextureLevel &level : out->levels)
		for (size_t i = 3; i < level.rgba.size(); i += 4) {
			const bool kept = use.inverted ? level.rgba[i] <= use.cutout : level.rgba[i] > use.cutout;
			level.rgba[i] = kept ? 255 : 0;
		}
	return out;
}

const char *texture_view_status_token(TextureViewStatus status) {
	switch (status) {
	case TextureViewStatus::NoTexture: return "no_texture";
	case TextureViewStatus::Unloadable: return "unloadable";
	case TextureViewStatus::Undecoded: return "undecoded";
	case TextureViewStatus::Ready: return "ready";
	}
	return "no_texture";
}

const char *texture_channels_token(TextureChannels channels) {
	const size_t index = static_cast<size_t>(channels);
	return index < std::size(kChannelTokens) ? kChannelTokens[index] : "rgba";
}

bool texture_channels_from_token(const std::string &token, TextureChannels &out) {
	for (size_t i = 0; i < std::size(kChannelTokens); ++i)
		if (token == kChannelTokens[i]) {
			out = static_cast<TextureChannels>(i);
			return true;
		}
	return false;
}

float texture_zoom_step(float scale, bool in) {
	if (in) {
		for (const float step : kTextureZoomSteps)
			if (step > scale * 1.001f) return step;
		return kMostScale;
	}
	for (size_t i = std::size(kTextureZoomSteps); i-- > 0;)
		if (kTextureZoomSteps[i] < scale * 0.999f) return kTextureZoomSteps[i];
	return kLeastScale;
}

std::string texture_camera_change(const TextureCamera &camera) {
	return viewport_change(ViewportKind::Texture, "camera", camera_to_json(camera));
}

std::string texture_options_change(const TextureViewportOptions &options) {
	return viewport_change(ViewportKind::Texture, "options", options_to_json(options));
}

TextureViewport::TextureViewport(std::string path) : ViewportModel(ViewportKind::Texture, std::move(path), kHeadlessSize) {}

std::unique_ptr<ViewportModel> TextureViewport::make(const std::string &path) {
	return std::make_unique<TextureViewport>(path);
}

ViewportStatus TextureViewport::status() const {
	switch (reason_) {
	case TextureViewStatus::Ready: return ViewportStatus::Ready;
	case TextureViewStatus::Unloadable:
	case TextureViewStatus::Undecoded: return ViewportStatus::Failed;
	case TextureViewStatus::NoTexture: break;
	}
	return ViewportStatus::Empty;
}

std::string TextureViewport::message() const {
	switch (reason_) {
	case TextureViewStatus::NoTexture: return "No texture is open or selected here.";
	case TextureViewStatus::Unloadable: return "The game cannot load this texture: " + detail_;
	case TextureViewStatus::Undecoded: return "The game loads this texture, but the editor does not decode " + detail_ + ".";
	case TextureViewStatus::Ready: break;
	}
	return std::string();
}

std::string TextureViewport::caption() const {
	if (!image_ || !image_->width()) return std::string();
	std::string out = " - " + std::to_string(image_->width()) + " x " + std::to_string(image_->height());
	if (const TextureFact *texels = image_->fact("texels")) {
		const std::string &words = texels->words;
		out += ", " + words.substr(0, words.find(':'));
	}
	if (image_->levels.size() > 1) out += ", " + std::to_string(image_->levels.size()) + " levels";
	return out;
}

size_t TextureViewport::shown_level() const {
	if (!image_ || image_->levels.empty()) return 0;
	return std::min(size_t(std::max(options_.level, 0)), image_->levels.size() - 1);
}

TexturePlacement TextureViewport::placement(int width, int height) const {
	TexturePlacement out;
	const float w = image_ ? float(image_->width()) : 0.0f, h = image_ ? float(image_->height()) : 0.0f;
	if (camera_.fit) {
		if (w > 0.0f && h > 0.0f && width > 0 && height > 0)
			out.scale = std::clamp(std::min(float(width) / w, float(height) / h), kLeastScale, kMostScale);
		out.x = w * 0.5f;
		out.y = h * 0.5f;
		return out;
	}
	out.scale = camera_.scale;
	out.x = camera_.x;
	out.y = camera_.y;
	return out;
}

bool TextureViewport::texel_at(float x, float y, int width, int height, uint32_t &tx, uint32_t &ty,
                               uint8_t rgba[4]) const {
	if (reason_ != TextureViewStatus::Ready || !image_ || image_->levels.empty()) return false;
	float fx = 0.0f, fy = 0.0f;
	placement(width, height).texel_of(x, y, width, height, fx, fy);
	if (fx < 0.0f || fy < 0.0f || fx >= float(image_->width()) || fy >= float(image_->height())) return false;
	const size_t level = shown_level();
	const TextureLevel &at = image_->levels[level];
	tx = std::min(at.width - 1, uint32_t(fx * float(at.width) / float(image_->width())));
	ty = std::min(at.height - 1, uint32_t(fy * float(at.height) / float(image_->height())));
	return texture_texel(*image_, level, tx, ty, rgba);
}

std::string TextureViewport::texel_words(uint32_t tx, uint32_t ty, const uint8_t rgba[4]) const {
	std::string out = std::to_string(tx) + ", " + std::to_string(ty) + ": R " + std::to_string(rgba[0]) + " G " +
	                  std::to_string(rgba[1]) + " B " + std::to_string(rgba[2]) + " A " + std::to_string(rgba[3]);
	if (image_ && shown_level() == 0 && !image_->indices.empty()) {
		const size_t at = size_t(ty) * image_->width() + tx;
		if (at < image_->indices.size()) out += " (palette entry " + std::to_string(image_->indices[at]) + ")";
	}
	// A compare's texel: the reference's and the compressed texture's, each.
	if (compression_ && compression_->made && options_.compare != TextureCompareView::Off) {
		const size_t level = shown_level();
		uint8_t before[4] = {}, after[4] = {};
		const auto words = [](const uint8_t v[4]) {
			return "R " + std::to_string(v[0]) + " G " + std::to_string(v[1]) + " B " + std::to_string(v[2]) + " A " +
			       std::to_string(v[3]);
		};
		if (texture_texel(*compression_->reference, level, tx, ty, before) &&
		    texture_texel(*compression_->compressed, level, tx, ty, after))
			out += "; before: " + words(before) + "; " + compression_->format + ": " + words(after);
	}
	return out;
}

const TextureLevelError *TextureViewport::shown_error() const {
	if (!compression_ || !compression_->made || options_.compare == TextureCompareView::Off) return nullptr;
	const size_t level = shown_level();
	return level < compression_->errors.size() ? &compression_->errors[level] : nullptr;
}

std::shared_ptr<const TextureCompression> TextureViewport::compare(const ViewportInput &input,
                                                                   const std::shared_ptr<const TextureImage> &image) {
	// A .dds is weighed against its import's source as that import prepares it, read again when the source moves.
	TextureImportState state;
	std::string error;
	const bool dds = image->reader == TextureReader::Dds;
	const bool imported = dds && input.view.project.open && texture_import_state(input.view, path(), state, error);
	const AssetEntry *source = imported && input.view.project.scan ? input.view.project.scan->at_path(state.source) : nullptr;
	if (kept_ && compared_ == image && (!source || (source->size_bytes == compared_source_size_ &&
	                                                source->modified_ticks == compared_source_modified_)))
		return kept_;
	compared_ = image;
	compared_source_size_ = source ? source->size_bytes : 0;
	compared_source_modified_ = source ? source->modified_ticks : 0;
	auto made = std::make_shared<TextureCompression>();
	if (!dds) {
		*made = compress_texture(*image);
	} else if (!source) {
		made->why = "A .dds holds its compressed texels alone, with no image of them to compare with; a .dds an import "
		            "makes compares against its import's source.";
	} else {
		std::vector<uint8_t> bytes;
		renderer::ImageSource decoded;
		std::string field;
		if (!io::read_file_bytes(join_path(input.view.project.root, state.source), bytes, error) ||
		    !renderer::decode_image_source(state.source, bytes, decoded, error) ||
		    !renderer::image_import_texels(decoded.image, renderer::image_import_settings(state.sidecar.options), error, field))
			made->why = "Its import's source " + state.source + " does not read as its import reads it: " + error;
		else
			*made = compare_dds(image, decoded.image, state.source + ", its import's source");
		if (const AssetEntry *file = input.view.project.scan ? input.view.project.scan->at_path(path()) : nullptr)
			made->file_bytes = file->size_bytes;
	}
	kept_ = made;
	return kept_;
}

ViewportAction TextureViewport::follow_(const ViewportInput &input, PreviewClock &) {
	std::shared_ptr<const TextureImage> image;
	bool from_file = false;
	if (input.document) {
		// The texture document open at its path: its image, read as the document was loaded.
		if (const auto *texture = dynamic_cast<const TextureDocument *>(input.document)) image = texture->image();
		file_read_ = false;
		file_image_.reset();
	} else if (input.view.project.open && input.view.project.scan) {
		// A file Files selects, not open: read as a texture document reads it, again only when the scan
		// says its size or last write moved.
		const AssetEntry *entry = input.view.project.scan->at_path(path());
		if (entry && (!file_read_ || entry->size_bytes != file_size_ || entry->modified_ticks != file_modified_)) {
			file_read_ = true;
			file_size_ = entry->size_bytes;
			file_modified_ = entry->modified_ticks;
			TextureDocument document;
			Diagnostic error;
			const std::string game = input.view.project.document ? input.view.project.document->target_game : std::string();
			file_image_ = document.load(join_path(input.view.project.root, path()), path(), entry->kind, game, error)
			                      ? document.image()
			                      : nullptr;
			++reads_;
		}
		if (!entry) {
			file_read_ = false;
			file_image_.reset();
		}
		image = file_image_;
		from_file = image != nullptr;
	}
	if (!image) {
		const bool had = image_ != nullptr;
		image_.reset();
		source_.reset();
		use_ = TextureShownUse();
		role_view_ = TextureRoleView();
		reason_ = TextureViewStatus::NoTexture;
		detail_.clear();
		from_file_ = false;
		shown_none();
		return had ? ViewportAction::Clear : ViewportAction::Keep;
	}
	// The use the options name, as the texture's uses stand (none past their count), and the budget the object
	// texture detail reads: that use's, else the first model row's.
	TextureShownUse use;
	const TextureBudget *budget = nullptr;
	std::string terrain_path;
	if (input.view.documents.texture_uses) {
		const std::vector<TextureUse> &uses = input.view.documents.texture_uses->uses_of(input.view, path());
		if (options_.as_used >= 0 && size_t(options_.as_used) < uses.size()) {
			const TextureUse &shown = uses[size_t(options_.as_used)];
			use = texture_shown_use(shown, options_.as_used);
			if (shown.budget.known) budget = &shown.budget;
			if (shown.role == TextureRoleId::TerrainBlendMap || shown.role == TextureRoleId::TerrainFoliageMap)
				terrain_path = shown.referrer;
		}
		for (size_t i = 0; i < uses.size() && !budget; ++i)
			if (uses[i].budget.known) budget = &uses[i].budget;
	}
	// The terrain a terrain map's use names it from, read as the scan last listed it.
	const AssetEntry *terrain_entry =
	        terrain_path.empty() || !input.view.project.scan ? nullptr : input.view.project.scan->at_path(terrain_path);
	const bool terrain_moved = terrain_path != terrain_path_ ||
	                           (terrain_entry && (terrain_entry->size_bytes != terrain_size_ || terrain_entry->modified_ticks != terrain_modified_));
	if (terrain_moved) {
		terrain_path_ = terrain_path;
		terrain_size_ = terrain_entry ? terrain_entry->size_bytes : 0;
		terrain_modified_ = terrain_entry ? terrain_entry->modified_ticks : 0;
		terrain_.reset();
		std::string text, error;
		if (terrain_entry && io::read_file_text(join_path(input.view.project.root, terrain_path), text, error)) {
			std::istringstream file(text);
			auto config = std::make_shared<TrnConfig>();
			if (load_trn(file, *config, error)) terrain_ = std::move(config);
		}
	}
	const bool read_anew = image != source_;
	// What moves the picture besides its texels and its use: the detail, the game's chain asked for, the
	// normals lit and their light.
	const bool lit = options_.channels == TextureChannels::Normals, was_lit = made_options_.channels == TextureChannels::Normals;
	const bool reshaped = options_.detail != made_options_.detail || (options_.level > 0) != (made_options_.level > 0) ||
	                      lit != was_lit || (lit && options_.light != made_options_.light) ||
	                      (options_.detail >= 0 && budget && !(budget->detail[options_.detail].bytes == detail_device_.bytes &&
	                                                           budget->detail[options_.detail].width == detail_device_.width));
	bool anew = read_anew || !(use == use_) || reshaped || terrain_moved;
	if (read_anew && input.document) ++reads_;
	source_ = image;
	use_ = use;
	// The compare the options ask for (S18): made of the file's texels, again only when they or a .dds's import
	// source move; its picture in place of the texture's while it is made.
	std::shared_ptr<const TextureCompression> compression;
	if (options_.compare != TextureCompareView::Off && image->loads && image->decoded) compression = compare(input, image);
	if (compression != compression_ || options_.compare != shown_compare_ || options_.split != shown_split_) anew = true;
	compression_ = compression;
	shown_compare_ = options_.compare;
	shown_split_ = options_.split;
	if (anew) {
		made_options_ = options_;
		const std::shared_ptr<const TextureImage> used = use.index >= 0 && image->decoded ? texture_as_used(image, use) : image;
		// What the use's role reads of it: the legend and the line the picture's words carry.
		role_view_ = use.index >= 0 && used ? texture_role_view(*image, *used, use.role, use.blend_mode, terrain_.get())
		                                    : TextureRoleView();
		image_ = picture(used, budget);
		if (compression_ && compression_->made)
			if (std::shared_ptr<const TextureImage> compared = texture_compare_picture(*compression_, options_.compare, options_.split))
				image_ = compared;
	}
	from_file_ = from_file;
	if (!image->loads) {
		reason_ = TextureViewStatus::Unloadable;
		detail_ = image->refusal;
	} else if (!image->decoded) {
		reason_ = TextureViewStatus::Undecoded;
		detail_ = image->undecoded;
	} else {
		reason_ = TextureViewStatus::Ready;
		detail_.clear();
	}
	if (input.document) shown(*input.document);
	else shown_none();
	if (!anew) return ViewportAction::Keep;
	return reason_ == TextureViewStatus::Ready ? ViewportAction::Rebuild : ViewportAction::Clear;
}

// [orig: the device texture: GTexture_CreateFromPixelData_0 @ 0x6876C0 (a texture built from pixels halved, then
// its chain the box filter of each level, @ 0x6878BE), GTexture_InitFromMemory @ 0x687DF0 (a DDS's top levels
// skipped); renderer/device_texture]
std::shared_ptr<const TextureImage> TextureViewport::picture(const std::shared_ptr<const TextureImage> &used,
                                                            const TextureBudget *budget) {
	detail_level_ = -1;
	detail_device_ = renderer::DeviceTexture();
	if (!used || !used->decoded || used->levels.empty() || used->levels.front().rgba.empty()) return used;
	std::shared_ptr<TextureImage> out;
	const auto own = [&]() -> TextureImage & {
		if (!out) out = std::make_shared<TextureImage>(*used);
		return *out;
	};
	const bool dds = used->reader == TextureReader::Dds;
	// The device texture at the detail asked for, by the use's loader and slot: a DDS's levels from the skip, a
	// texture of pixels its first level halved as the game halves it.
	if (options_.detail >= 0 && budget && budget->known) {
		detail_level_ = options_.detail;
		detail_device_ = budget->detail[options_.detail];
		TextureImage &image = own();
		image.indices.clear();
		if (dds) {
			if (!detail_device_.whole && detail_device_.halvings > 0 && detail_device_.halvings < image.levels.size())
				image.levels.erase(image.levels.begin(), image.levels.begin() + std::ptrdiff_t(detail_device_.halvings));
		} else {
			TextureLevel top = texture_halved(image.levels.front(), detail_device_.halvings);
			image.levels.assign(1, std::move(top));
		}
	}
	// The chain the game builds of a texture made from pixels, once a level past the first is asked for.
	if (!dds && options_.level > 0 && (out ? out->levels.size() : used->levels.size()) == 1) {
		TextureImage &image = own();
		const TextureLevel &first = image.levels.front();
		image.levels = texture_game_chain(first, renderer::texture_level_count(first.width, first.height, 0));
	}
	// A normal map lit.
	if (options_.channels == TextureChannels::Normals) {
		TextureImage &image = own();
		image.indices.clear();
		for (TextureLevel &level : image.levels) level = texture_lit_normals(level, options_.light);
		image.alpha = TextureAlpha::None;
	}
	return out ? std::shared_ptr<const TextureImage>(out) : used;
}

bool TextureViewport::takes_(const std::string &member) const {
	return member == "options" || member == "camera";
}

bool TextureViewport::check_(const io::JsonValue &json, std::string &error) const {
	TextureViewportOptions options = options_;
	if (const JsonValue *member = json.get("options"); member && !read_options(*member, options, error)) return false;
	TextureCamera camera = camera_;
	if (const JsonValue *member = json.get("camera"); member && !read_camera(*member, camera, error)) return false;
	return true;
}

void TextureViewport::apply_(const io::JsonValue &json, PreviewClock &) {
	std::string error;
	if (const JsonValue *member = json.get("options")) read_options(*member, options_, error);
	if (const JsonValue *member = json.get("camera")) read_camera(*member, camera_, error);
}

std::unique_ptr<CanvasHalf> TextureViewport::make_canvas() const {
	return std::make_unique<TextureCanvas>();
}

ViewportHit TextureViewport::hit(const ViewportContext &context, float x, float y) const {
	ViewportHit out;
	out.current = current(context.input) || from_file_;
	uint32_t tx = 0, ty = 0;
	uint8_t rgba[4] = {};
	if (!texel_at(x, y, context.width, context.height, tx, ty, rgba)) return out;
	out.index = int(size_t(ty) * image_->levels[shown_level()].width + tx);
	out.name = texel_words(tx, ty, rgba);
	out.kind = "texel";
	return out;
}

bool TextureViewport::handle_point(const ViewportContext &, NodeId, const std::string &, float &, float &,
                                   std::string &error) const {
	error = "A texture's picture has no handles: a texture holds no records.";
	return false;
}

bool TextureViewport::drag(const ViewportContext &, const ViewportDrag &, CanvasRequests &, std::string &error) const {
	error = "Nothing is dragged in a texture's picture: its camera is set with set_viewport.";
	return false;
}

bool TextureViewport::command(const ViewportContext &context, const std::string &name, const std::vector<NodeId> &,
                              CanvasRequests &out, std::string &error) const {
	TextureCamera camera = camera_;
	if (name == "fit") {
		camera.fit = true;
	} else if (name == "actual") {
		const TexturePlacement now = placement(context.width, context.height);
		camera.fit = false;
		camera.scale = 1.0f;
		camera.x = now.x;
		camera.y = now.y;
	} else {
		error = "A texture's picture has no command \"" + name + "\" (fit, actual).";
		return false;
	}
	out.request(request::set_viewport(path(), texture_camera_change(camera)));
	return true;
}

io::JsonValue TextureViewport::options_json() const {
	return options_to_json(options_);
}

io::JsonValue TextureViewport::camera_json() const {
	return camera_to_json(camera_);
}

io::JsonValue TextureViewport::body_json(const ViewportInput &) const {
	JsonValue out = image_ ? texture_image_json(*image_) : JsonValue::make_object();
	out.set("from_file", JsonValue::make_bool(from_file_));
	// The use shown: its index and words, what its loader makes of the texels, the cut-out and the cells.
	JsonValue as_used = JsonValue::make_null();
	if (use_.index >= 0) {
		as_used = JsonValue::make_object();
		as_used.set("index", json_number(use_.index));
		as_used.set("words", json_string(use_.words));
		as_used.set("role", json_string(use_.role == TextureRoleId::kCount ? "" : texture_role_row(use_.role).token));
		as_used.set("transform", json_string(texture_load_transform_token(use_.transform)));
		as_used.set("cutout", json_number(use_.cutout));
		as_used.set("inverted", JsonValue::make_bool(use_.inverted));
		as_used.set("cells", json_number(double(use_.cells)));
		if (use_.model_row) as_used.set("alpha", json_string(texture_alpha_meaning_token(use_.alpha)));
		as_used.set("alpha_words", json_string(use_.alpha_words));
		if (use_.blend_mode >= 0) as_used.set("blend_mode", json_number(use_.blend_mode));
		// What its role reads of it (texture_role_view), null for none.
		as_used.set("role_view", role_view_.empty() ? JsonValue::make_null() : texture_role_view_json(role_view_));
	}
	out.set("as_used", std::move(as_used));
	// The device texture shown at the detail asked for, and the levels the picture holds (a texture of pixels' the
	// chain the game builds once a level past the first is asked for).
	JsonValue detail = JsonValue::make_null();
	if (detail_level_ >= 0) {
		detail = JsonValue::make_object();
		detail.set("level", json_number(detail_level_));
		detail.set("width", json_number(double(detail_device_.width)));
		detail.set("height", json_number(double(detail_device_.height)));
		detail.set("format", json_string(renderer::device_texture_format_name(detail_device_.format)));
		detail.set("levels", json_number(double(detail_device_.levels)));
		detail.set("halvings", json_number(double(detail_device_.halvings)));
		detail.set("whole", JsonValue::make_bool(detail_device_.whole));
		detail.set("bytes", json_number(double(detail_device_.bytes)));
	}
	out.set("detail", std::move(detail));
	out.set("picture_levels", json_number(image_ ? double(image_->levels.size()) : 0.0));
	out.set("compare", compression_ ? texture_compression_json(*compression_) : JsonValue::make_null());
	out.set("level_shown", json_number(double(shown_level())));
	const ViewportState at = size();
	const TexturePlacement placed = placement(at.width, at.height);
	JsonValue shown = JsonValue::make_object();
	shown.set("scale", json_number(placed.scale));
	shown.set("x", json_number(placed.x));
	shown.set("y", json_number(placed.y));
	out.set("placement", std::move(shown));
	return out;
}

io::JsonValue TextureViewport::items_json(const ViewportInput &) const {
	return JsonValue::make_array();
}

} // namespace opennova::editor
