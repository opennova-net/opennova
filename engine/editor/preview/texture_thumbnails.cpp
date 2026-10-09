#include <editor/preview/texture_thumbnails.h>

#include <algorithm>
#include <cmath>

#include <base/io/base64.h>
#include <editor/assets/asset_registry.h>
#include <editor/import/png_encode.h>
#include <editor/project/project_files.h>
#include <editor/session/view/session_view.h>

namespace opennova::editor {

namespace {

std::string fact_words(const TextureImage &image, const char *key) {
	const TextureFact *fact = image.fact(key);
	return fact ? fact->words : std::string();
}

// The picture's size: the source's within `side` a side, its proportions kept, a side never 0.
void fit(uint32_t width, uint32_t height, uint32_t side, uint32_t &out_width, uint32_t &out_height) {
	if (width <= side && height <= side) {
		out_width = width;
		out_height = height;
		return;
	}
	const double scale = double(side) / double(std::max(width, height));
	out_width = std::max<uint32_t>(1, uint32_t(std::lround(width * scale)));
	out_height = std::max<uint32_t>(1, uint32_t(std::lround(height * scale)));
}

// Each pixel the average of the texels it covers, its colour weighted by their alpha (a texel the alpha
// hides adds none of its colour; where every one is hidden, their plain average).
std::vector<uint8_t> shrink(const TextureLevel &level, uint32_t width, uint32_t height) {
	std::vector<uint8_t> out(size_t(width) * height * 4);
	for (uint32_t y = 0; y < height; ++y) {
		const uint32_t y0 = uint32_t(uint64_t(y) * level.height / height);
		const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * level.height / height));
		for (uint32_t x = 0; x < width; ++x) {
			const uint32_t x0 = uint32_t(uint64_t(x) * level.width / width);
			const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * level.width / width));
			uint64_t weighted[3] = {}, plain[3] = {}, alpha = 0, count = 0;
			for (uint32_t sy = y0; sy < y1 && sy < level.height; ++sy)
				for (uint32_t sx = x0; sx < x1 && sx < level.width; ++sx) {
					const uint8_t *t = &level.rgba[(size_t(sy) * level.width + sx) * 4];
					for (int c = 0; c < 3; ++c) {
						weighted[c] += uint64_t(t[c]) * t[3];
						plain[c] += t[c];
					}
					alpha += t[3];
					++count;
				}
			uint8_t *p = &out[(size_t(y) * width + x) * 4];
			if (!count) continue;
			for (int c = 0; c < 3; ++c) p[c] = uint8_t(alpha ? (weighted[c] + alpha / 2) / alpha : (plain[c] + count / 2) / count);
			p[3] = uint8_t((alpha + count / 2) / count);
		}
	}
	return out;
}

} // namespace

std::shared_ptr<TextureThumbnail> make_texture_thumbnail(const std::string &file, const std::vector<uint8_t> &bytes,
                                                         TextureLoadTransform transform, uint32_t side) {
	auto out = std::make_shared<TextureThumbnail>();
	out->file = file;
	out->transform = transform;
	// The first level alone decoded (a DDS chain's others left coded: the picture is made from the top).
	std::shared_ptr<const TextureImage> image = decode_texture(file, bytes, true);
	out->format = fact_words(*image, "format");
	out->texels = fact_words(*image, "texels");
	out->alpha = fact_words(*image, "alpha");
	out->levels = image->reader == TextureReader::Dds ? size_t(image->game_levels) : image->levels.size();
	out->source_width = image->width();
	out->source_height = image->height();
	if (!image->loads || !image->decoded || image->levels.empty()) {
		out->state = TextureThumbnail::State::Unloadable;
		out->refusal = !image->loads ? image->refusal : image->undecoded;
		return out;
	}
	if (transform != TextureLoadTransform::None) image = apply_load_transform(*image, transform);
	const TextureLevel &source = image->levels.front();
	fit(source.width, source.height, std::max<uint32_t>(side, 1), out->width, out->height);
	out->rgba = shrink(source, out->width, out->height);
	uint64_t sum[4] = {};
	const size_t pixels = out->rgba.size() / 4;
	for (size_t i = 0; i < pixels; ++i) {
		for (int c = 0; c < 4; ++c) sum[c] += out->rgba[i * 4 + c];
		out->translucent = out->translucent || out->rgba[i * 4 + 3] < 255;
	}
	for (int c = 0; c < 4 && pixels; ++c) out->average[c] = uint8_t(sum[c] / pixels);
	return out;
}

std::shared_ptr<const TextureThumbnail> TextureThumbnails::get(const SessionView &view, const std::string &file,
                                                               TextureLoadTransform transform) const {
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(file) : nullptr;
	if (!entry) return nullptr;
	const Key key{file, transform};
	const auto found = entries_.find(key);
	const bool current =
			found != entries_.end() && found->second.size == entry->size_bytes && found->second.modified == entry->modified_ticks;
	if (found != entries_.end()) found->second.used = ++clock_;
	if (!current && std::find(queue_.begin(), queue_.end(), key) == queue_.end()) queue_.push_back(key);
	return found != entries_.end() ? found->second.picture : nullptr;
}

std::shared_ptr<const TextureThumbnail> TextureThumbnails::make_(const SessionView &view, const Key &key, size_t *read) {
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(key.first) : nullptr;
	if (!entry) {
		entries_.erase(key);
		return nullptr;
	}
	std::vector<uint8_t> bytes;
	std::string error;
	if (!io::read_file_bytes(join_path(view.project.root, entry->relative_path), bytes, error)) {
		// Not read while its stamp stands: asked again only once the file moves.
		Entry &slot = entries_[key];
		if (slot.picture) held_ -= slot.picture->rgba.size();
		slot = Entry{entry->size_bytes, entry->modified_ticks, nullptr, ++clock_};
		return nullptr;
	}
	if (read) *read += bytes.size();
	std::shared_ptr<TextureThumbnail> picture = make_texture_thumbnail(entry->relative_path, bytes, key.second);
	picture->serial = ++serial_;
	Entry &slot = entries_[key];
	if (slot.picture) held_ -= slot.picture->rgba.size();
	slot.size = entry->size_bytes;
	slot.modified = entry->modified_ticks;
	slot.picture = picture;
	slot.used = ++clock_;
	held_ += picture->rgba.size();
	trim_();
	return picture;
}

std::shared_ptr<const TextureThumbnail> TextureThumbnails::make_now(const SessionView &view, const std::string &file,
                                                                    TextureLoadTransform transform) {
	const Key key{file, transform};
	const AssetEntry *entry = view.project.scan ? view.project.scan->at_path(file) : nullptr;
	const auto found = entries_.find(key);
	if (entry && found != entries_.end() && found->second.size == entry->size_bytes &&
	    found->second.modified == entry->modified_ticks) {
		found->second.used = ++clock_;
		return found->second.picture;
	}
	queue_.erase(std::remove(queue_.begin(), queue_.end(), key), queue_.end());
	return make_(view, key, nullptr);
}

std::shared_ptr<const TextureThumbnail> TextureThumbnails::picture_of(const std::string &name, const std::vector<uint8_t> &bytes) {
	std::shared_ptr<TextureThumbnail> picture = make_texture_thumbnail(name, bytes, TextureLoadTransform::None);
	picture->serial = ++serial_;
	return picture;
}

bool TextureThumbnails::step(const SessionView &view, size_t bytes, const std::function<int64_t()> &clock, int64_t until) {
	bool made = false;
	size_t read = 0;
	const auto in_time = [&] { return !clock || clock() < until; };
	while (!queue_.empty() && (!made || (read < bytes && in_time()))) {
		const Key key = queue_.front();
		queue_.erase(queue_.begin());
		made = make_(view, key, &read) != nullptr || made;
	}
	return made;
}

void TextureThumbnails::trim_() {
	while (held_ > kBudget && entries_.size() > 1) {
		auto oldest = entries_.begin();
		for (auto it = entries_.begin(); it != entries_.end(); ++it)
			if (it->second.used < oldest->second.used) oldest = it;
		if (oldest->second.picture) held_ -= oldest->second.picture->rgba.size();
		entries_.erase(oldest);
	}
}

void TextureThumbnails::clear() {
	entries_.clear();
	queue_.clear();
	held_ = 0;
}

std::vector<uint8_t> thumbnail_png(const TextureThumbnail &thumbnail) {
	if (thumbnail.state != TextureThumbnail::State::Ready || thumbnail.rgba.empty()) return {};
	return encode_png_rgba(thumbnail.rgba.data(), thumbnail.width, thumbnail.height);
}

io::JsonValue texture_thumbnail_json(const TextureThumbnail &thumbnail, bool png) {
	using io::json_number;
	using io::json_string;
	io::JsonValue out = io::JsonValue::make_object();
	out.set("file", json_string(thumbnail.file));
	out.set("state", json_string(thumbnail.state == TextureThumbnail::State::Ready ? "ready" : "unloadable"));
	out.set("transform", json_string(texture_load_transform_token(thumbnail.transform)));
	out.set("width", json_number(double(thumbnail.width)));
	out.set("height", json_number(double(thumbnail.height)));
	out.set("source_width", json_number(double(thumbnail.source_width)));
	out.set("source_height", json_number(double(thumbnail.source_height)));
	out.set("levels", json_number(double(thumbnail.levels)));
	out.set("format", json_string(thumbnail.format));
	out.set("texels", json_string(thumbnail.texels));
	out.set("alpha", json_string(thumbnail.alpha));
	if (!thumbnail.refusal.empty()) out.set("refusal", json_string(thumbnail.refusal));
	if (png) {
		const std::vector<uint8_t> bytes = thumbnail_png(thumbnail);
		if (!bytes.empty()) out.set("png", json_string(io::base64_encode(bytes)));
	}
	return out;
}

} // namespace opennova::editor
