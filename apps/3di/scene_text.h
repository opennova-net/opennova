// opennova-3di scene text: the tokenizer, field readers and printers the two
// scene grammars share, the `.o3d` model text (build.cpp reads it, scene.cpp
// writes it; docs/threedi/o3d-scene-format.md) and the `.o3a` clip-set text
// (anim_build.cpp, anim_scene.cpp; docs/anim/o3a-scene-format.md), so what one
// writes the other reads by the same rules. What differs between the two is a
// parameter: the `.o3d` reader takes nan and inf, which retail models carry
// (J_bsh1's vertex normals, ChmLFP1's occlusion planes), and the `.o3a` reader
// refuses every number that is not finite (a clip holds none).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace opennova::threedi_cli {

// Which numbers a text's reader takes.
enum class SceneNumbers {
	any,    // what strtod reads, plus nan and -nan as retail's quiet NaN
	finite, // finite numbers only
};

// One record's fields: whitespace-separated tokens (space, tab, \r, \v, \f);
// a token that opens with '"' runs to the next '"' and may hold spaces or be
// empty (retail user points `FLARE 01` and `ground `, an empty CTRL name, a
// bone `BN01 Pelvis`). A quoted token must end at whitespace, and a bare one
// cannot hold '"': `bad` says why a line does not split.
struct SceneLine {
	std::vector<std::string> tokens;
	size_t next = 1; // after the record key
	std::string bad; // why the line does not split into tokens

	SceneLine(const std::string &text, SceneNumbers numbers);

	const std::string &key() const { return tokens[0]; }
	bool more() const { return next < tokens.size(); }
	const std::string &peek() const { return tokens[next]; }

	// A number, the whole token. Under SceneNumbers::any, also `nan` / `-nan`
	// as the quiet NaN retail stores (0x7FC00000 and its negation: J_bsh1,
	// ChmLFP1), whatever payload the C library's strtod gives the word
	// (MSVC's sets every mantissa bit).
	bool number(double &out);
	bool numbers(double *out, int n);
	// A whole number in decimal, or in hex after 0x (flag words); a leading 0
	// is not octal, and a value past a long long is refused, not clamped.
	bool integer(long long &out);
	// A whole number in [lo, hi].
	bool integer(long long &out, long long lo, long long hi);
	bool name(std::string &out);

private:
	SceneNumbers numbers_;
};

// The line without its comment: `#` at the start of a line or after
// whitespace (retail shader tags such as `VS_PHONGT#UV` hold a '#'), never
// inside quotes.
std::string strip_comment(const std::string &line);

// A number as the readers and Python's float() both read it: `format`, with
// -0 as 0, and NaN and infinity spelled nan, -nan, inf and -inf (MSVC's printf
// writes "-nan(ind)", which Python refuses).
std::string number_text(double v, const char *format);
// Floats print with 9 significant digits (a float32 round-trips exactly);
// values from fixed-point words, and doubles a builder turns back into floats,
// print with 17 (exact re-quantization).
std::string f9(double v);
std::string f17(double v);

// A name field: bare when it is one plain token, else "quoted" (both readers
// take either; any whitespace, \v and \r included, is quoted, and so is a name
// a '#' would open as a comment). A name cannot hold '"' or a line break:
// those characters are left out, and `kept` receives the name the field holds
// (shorter than `name` when any was).
std::string name_field(const std::string &name, std::string &kept);

// name_field for a scene writer, whose note() records a name it shortened.
template <typename SceneWriter>
std::string name_field(SceneWriter &w, const std::string &name) {
	std::string kept;
	std::string field = name_field(name, kept);
	if (kept.size() != name.size())
		w.note("the name '" + kept + "' held a '\"' or a line break (written without it)");
	return field;
}

} // namespace opennova::threedi_cli
