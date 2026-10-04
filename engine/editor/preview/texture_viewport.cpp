#include <editor/preview/texture_viewport.h>

#include <algorithm>
#include <cmath>
#include <iterator>

#include <editor/assets/asset_registry.h>
#include <editor/documents/texture_document.h>
#include <editor/preview/texture_canvas.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/request_factories.h>
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

constexpr const char *kChannelTokens[] = {"rgb", "red", "green", "blue", "alpha", "rgba"};

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
	return out;
}

// A SetViewport's options: {channels, level, as_used}, each optional.
bool read_options(const JsonValue &json, TextureViewportOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options is an object {channels, level, as_used}.";
		return false;
	}
	TextureViewportOptions options = held;
	for (const io::JsonMember &member : json.object) {
		if (member.key == "channels") {
			if (!member.value.is_string() || !texture_channels_from_token(member.value.string, options.channels)) {
				error = "options.channels is one of rgb, red, green, blue, alpha, rgba.";
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
		} else {
			error = "Unknown options member \"" + member.key + "\" (it takes channels, level, as_used).";
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
	if ((use.role == TextureRoleId::ModelDiffuse || use.role == TextureRoleId::ModelFlipFrame) && use.context.alpha_test()) {
		out.cutout = use.context.alpha_ref;
		out.inverted = use.context.alpha_test_inverted();
	}
	// A tile atlas is cut in 64-texel cells [orig: Terrain_LoadTileSetAtlas @ 0x604B7C].
	if (use.role == TextureRoleId::TerrainTileAtlas) out.cells = 64;
	return out;
}

std::shared_ptr<const TextureImage> texture_as_used(const std::shared_ptr<const TextureImage> &image,
                                                    const TextureShownUse &use) {
	if (!image) return image;
	std::shared_ptr<const TextureImage> made =
	        use.transform == TextureLoadTransform::None ? image : apply_load_transform(*image, use.transform);
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
	return out;
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
		reason_ = TextureViewStatus::NoTexture;
		detail_.clear();
		from_file_ = false;
		shown_none();
		return had ? ViewportAction::Clear : ViewportAction::Keep;
	}
	// The use the options name, as the texture's uses stand (none past their count).
	TextureShownUse use;
	if (options_.as_used >= 0 && input.view.documents.texture_uses) {
		const std::vector<TextureUse> &uses = input.view.documents.texture_uses->uses_of(input.view, path());
		if (size_t(options_.as_used) < uses.size()) use = texture_shown_use(uses[size_t(options_.as_used)], options_.as_used);
	}
	const bool read_anew = image != source_;
	const bool anew = read_anew || !(use == use_);
	if (read_anew && input.document) ++reads_;
	source_ = image;
	use_ = use;
	if (anew) image_ = use.index >= 0 && image->decoded ? texture_as_used(image, use) : image;
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
	}
	out.set("as_used", std::move(as_used));
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
