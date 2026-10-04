#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/io/json.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_load_rules.h>

namespace opennova::editor {

struct SessionView;

// A texture as a small picture (ADR 0046 S18): a project file read by the reader its name picks, what
// the use's loader makes of its texels applied (TextureLoadTransform: the HUD's alpha-only art, a sky's
// luminance alpha), the level nearest above the picture's size shrunk to fit `side` a side by averaging
// (by alpha, so a cut-out's hidden colour does not bleed in), with what the file is in a few words.
struct TextureThumbnail {
	enum class State : uint8_t {
		Ready,      // the picture made
		Unloadable, // the game cannot load the file (refusal says why); no picture
	};
	State state = State::Ready;
	std::string file; // project-relative
	TextureLoadTransform transform = TextureLoadTransform::None;
	uint32_t width = 0, height = 0; // the picture's
	std::vector<uint8_t> rgba;      // RGBA8, rows top first
	uint32_t source_width = 0, source_height = 0;
	size_t levels = 0;      // the levels the file holds
	std::string format;     // the file's format in words ("DDS (DirectDraw surface)")
	std::string texels;     // its texels in words ("DXT5: 8 bits a texel, in 4 x 4 blocks")
	std::string alpha;      // its alpha in words
	std::string refusal;    // why the game cannot load it ("" when it can)
	uint64_t serial = 0;    // one per picture made, from 1: a device uploads each once
	bool translucent = false;  // a pixel of the picture is less than opaque (drawn over a checkerboard)
	uint8_t average[4] = {};   // the picture's average colour (a frame's stand-in with no device)
};

inline constexpr uint32_t kThumbnailSide = 128;

// The picture of a texture's bytes (`file` its name, which picks the reader), `side` pixels a side at
// most.
std::shared_ptr<TextureThumbnail> make_texture_thumbnail(const std::string &file, const std::vector<uint8_t> &bytes,
                                                         TextureLoadTransform transform, uint32_t side = kThumbnailSide);

// The thumbnails of the project's textures (ADR 0046 S18): each made once while its file's stamp
// stands (its size and last write as the scan read them), kept in a cache bounded in bytes (the least
// recently asked let go past kBudget), and made off the frame: one asked for and not made is queued,
// the last picture of its file (if any) answering meanwhile, and step() makes the queue's within a
// poll's budget. The windows ask (get) as they draw; the wire makes one at once (make_now).
class TextureThumbnails {
public:
	static constexpr size_t kBudget = size_t(32) << 20;

	// The picture of `file` (project-relative) as `transform` makes it: the one made while its stamp
	// stands; else the file is queued, and the last made of it answers (null for none yet, and for a
	// file the project does not have).
	std::shared_ptr<const TextureThumbnail> get(const SessionView &view, const std::string &file,
	                                            TextureLoadTransform transform) const;
	// The picture made now where it is not (null for a file the project does not have or that does not
	// read): what the wire answers with.
	std::shared_ptr<const TextureThumbnail> make_now(const SessionView &view, const std::string &file,
	                                                 TextureLoadTransform transform);
	// The picture of bytes the project holds no file of yet (S18: what a Replace would make), named `name`,
	// made now, never kept, with a serial of the cache's own (a device uploads each picture once).
	std::shared_ptr<const TextureThumbnail> picture_of(const std::string &name, const std::vector<uint8_t> &bytes);
	// Makes the queued pictures, one at least, until `bytes` of files are read or `clock` (when given) passes
	// `until` (the poll's budget): true when one was made.
	bool step(const SessionView &view, size_t bytes, const std::function<int64_t()> &clock = {}, int64_t until = 0);
	bool pending() const { return !queue_.empty(); }
	size_t held_bytes() const { return held_; }
	size_t held() const { return entries_.size(); }
	// How many pictures were made (a test counts the reads a stamp saves).
	uint64_t made() const { return serial_; }
	// The project closed: every picture let go.
	void clear();

private:
	using Key = std::pair<std::string, TextureLoadTransform>;
	struct Entry {
		uint64_t size = 0;
		int64_t modified = 0;
		std::shared_ptr<const TextureThumbnail> picture;
		uint64_t used = 0;
	};
	// The picture made for `key` from the file the scan lists now (null where it does not read).
	std::shared_ptr<const TextureThumbnail> make_(const SessionView &view, const Key &key, size_t *read);
	void trim_();

	mutable std::map<Key, Entry> entries_;
	mutable std::vector<Key> queue_;
	mutable uint64_t clock_ = 0;
	size_t held_ = 0;
	uint64_t serial_ = 0;
};

// A thumbnail's picture as a PNG (the wire's): empty for one with none.
std::vector<uint8_t> thumbnail_png(const TextureThumbnail &thumbnail);
// A thumbnail on the wire: file, state (ready, unloadable), transform, the picture's width and height,
// the file's source_width, source_height, levels, format, texels and alpha in words, refusal; with
// `png`, the picture as a base64 PNG.
io::JsonValue texture_thumbnail_json(const TextureThumbnail &thumbnail, bool png);

} // namespace opennova::editor
