#pragma once

// A raster of a test's own form (ADR 0046 S25, the raster core: RasterDocument over a codec), for the tests of
// the raster history and document and of what the editor does with a raster: "RB", the width and height (u16
// each), the plane's texel layout (u8: a RasterFormat), a note's length (u8) and the note (the side state),
// then the texels row-major. Its edits fill a rectangle (one may join the last step: amends), set the note,
// grow the plane (a whole change) and refuse after writing. A file not starting "RB" is no raster of it.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/finding_code_row.h>
#include <editor/model/raster_document.h>

namespace editor_test {

// The note beside the texels: what the side state carries (part bit 1 when it changes).
struct RasterNote : opennova::editor::RasterSideState {
	std::string text;
	explicit RasterNote(std::string note) : text(std::move(note)) {}
	size_t footprint() const override { return sizeof(*this) + text.size(); }
	uint32_t parts_changed(const opennova::editor::RasterSideState &before) const override {
		const auto *other = dynamic_cast<const RasterNote *>(&before);
		return other && other->text == text ? 0u : 1u;
	}
};

class RasterBlobCodec : public opennova::editor::RasterCodec {
public:
	bool read(const std::vector<uint8_t> &decoded, opennova::editor::RasterState &out,
	          std::vector<opennova::editor::SourceIssue> &issues, opennova::editor::Diagnostic &error) const override {
		(void)issues;
		using namespace opennova::editor;
		if (decoded.size() < 8) {
			error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, "The raster ends early.");
			return false;
		}
		const int width = decoded[2] | (decoded[3] << 8), height = decoded[4] | (decoded[5] << 8);
		const auto format = static_cast<RasterFormat>(decoded[6]);
		const size_t note = decoded[7];
		const size_t texels = static_cast<size_t>(width) * height * static_cast<size_t>(raster_texel_bytes(format));
		if (decoded.size() != 8 + note + texels) {
			error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error, "The raster's size is wrong.");
			return false;
		}
		out.planes = {RasterPlane::from_bytes(format, width, height, decoded.data() + 8 + note)};
		out.side = std::make_shared<RasterNote>(std::string(decoded.begin() + 8, decoded.begin() + 8 + long(note)));
		return true;
	}
	opennova::editor::SerializeResult write(const opennova::editor::RasterState &state) const override {
		opennova::editor::SerializeResult out;
		const opennova::editor::RasterPlane &plane = state.planes.at(0);
		const auto *note = dynamic_cast<const RasterNote *>(state.side.get());
		const std::string text = note ? note->text : std::string();
		out.text = std::string("RB");
		for (int v : {plane.width(), plane.height()}) {
			out.text.push_back(static_cast<char>(v & 0xFF));
			out.text.push_back(static_cast<char>(v >> 8));
		}
		out.text.push_back(static_cast<char>(plane.format()));
		out.text.push_back(static_cast<char>(text.size()));
		out.text += text;
		const std::vector<uint8_t> texels = plane.bytes();
		out.text.append(texels.begin(), texels.end());
		return out;
	}
	std::string save_words(const opennova::editor::RasterState &state) const override {
		(void)state;
		return "Writes the raster as it holds it.";
	}
};

inline std::shared_ptr<const opennova::editor::RasterCodec> pick_raster_blob(const std::string &path,
                                                                             const std::vector<uint8_t> &decoded) {
	(void)path;
	if (decoded.size() < 2 || decoded[0] != 'R' || decoded[1] != 'B') return nullptr;
	static const auto codec = std::make_shared<RasterBlobCodec>();
	return codec;
}

// The file of a w x h raster of `format`, every texel byte `fill`, with `note`.
inline std::vector<uint8_t> raster_blob_file(int w, int h, opennova::editor::RasterFormat format, uint8_t fill,
                                             const std::string &note = "note") {
	std::vector<uint8_t> out = {'R', 'B', uint8_t(w), uint8_t(w >> 8), uint8_t(h), uint8_t(h >> 8),
	                            uint8_t(format), uint8_t(note.size())};
	out.insert(out.end(), note.begin(), note.end());
	out.resize(out.size() + size_t(w) * h * size_t(opennova::editor::raster_texel_bytes(format)), fill);
	return out;
}

// Every byte of each texel in [x, x + w) x [y, y + h) of plane 0 set to `value`.
struct RasterFill : opennova::editor::RasterEditPayload {
	int x, y, w, h;
	uint8_t value;
	bool amend;
	RasterFill(int x0, int y0, int width, int height, uint8_t v, bool amends_last = false)
			: x(x0), y(y0), w(width), h(height), value(v), amend(amends_last) {}
	const char *token() const override { return "raster_blob.fill"; }
	bool amends() const override { return amend; }
	bool apply(opennova::editor::RasterCanvas &canvas, opennova::editor::Diagnostic &error) const override {
		(void)error;
		opennova::editor::RasterWriter &writer = canvas.write(0);
		const int bytes = canvas.plane(0).texel_bytes();
		for (int yy = y; yy < y + h; ++yy)
			for (int xx = x; xx < x + w; ++xx) std::fill_n(writer.texel(xx, yy), bytes, value);
		return true;
	}
};

struct RasterSetNote : opennova::editor::RasterEditPayload {
	std::string text;
	explicit RasterSetNote(std::string note) : text(std::move(note)) {}
	const char *token() const override { return "raster_blob.note"; }
	bool apply(opennova::editor::RasterCanvas &canvas, opennova::editor::Diagnostic &error) const override {
		(void)error;
		canvas.set_side(std::make_shared<RasterNote>(text));
		return true;
	}
};

// The plane made `w` x `h` (a whole change), its texels kept where both have them.
struct RasterGrow : opennova::editor::RasterEditPayload {
	int w, h;
	RasterGrow(int width, int height) : w(width), h(height) {}
	const char *token() const override { return "raster_blob.grow"; }
	bool apply(opennova::editor::RasterCanvas &canvas, opennova::editor::Diagnostic &error) const override {
		(void)error;
		using namespace opennova::editor;
		const RasterPlane &old = canvas.plane(0);
		const int bytes = old.texel_bytes();
		std::vector<uint8_t> texels(size_t(w) * h * size_t(bytes), 0);
		for (int yy = 0; yy < std::min(h, old.height()); ++yy)
			for (int xx = 0; xx < std::min(w, old.width()); ++xx)
				std::copy_n(old.texel(xx, yy), bytes, texels.begin() + (long(yy) * w + xx) * bytes);
		RasterState grown;
		grown.planes = {RasterPlane::from_bytes(old.format(), w, h, texels.data())};
		grown.side = canvas.side_shared();
		canvas.replace_whole(std::move(grown));
		return true;
	}
};

// Writes texel (0, 0), then refuses.
struct RasterRefuse : opennova::editor::RasterEditPayload {
	const char *token() const override { return "raster_blob.refuse"; }
	bool apply(opennova::editor::RasterCanvas &canvas, opennova::editor::Diagnostic &error) const override {
		*canvas.write(0).texel(0, 0) = 0xEE;
		error = opennova::editor::make_finding(opennova::editor::CoreFinding::DocumentValue,
		                                       opennova::editor::DiagnosticSeverity::Error, "Refused after writing.");
		return false;
	}
};

inline opennova::editor::Edit raster_edit(std::shared_ptr<const opennova::editor::RasterEditPayload> payload,
                                          uint64_t gesture = 0) {
	return opennova::editor::RasterDocument::edit(std::move(payload), gesture);
}

} // namespace editor_test
