#include <formats/particle/parser.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <base/io/ascii_config.h>
#include <base/io/crt_ftol.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>

namespace opennova::particle {

namespace {

using Section = ParticleReadState::Section;
using Cursor = ParticleReadState::Cursor;

// ------------------------------------------------------------ the line's tokens

// The separators the readers' token helper skips alone and strips from a token: ';', ',' and '=', in
// that order [orig: CParticleManager_Construct @ 0x5e897c, `mov dword_2C06288, 3D2C3Bh`].
constexpr char kSeparators[] = {';', ',', '='};

bool is_separator(char c) noexcept {
	return c == ';' || c == ',' || c == '=';
}

// Every separator taken out of the token in place; `removed` the last one taken. A separator still in
// the token after its first is taken runs the whole strip again from the first separator [orig:
// CParticleDefEntry_StripSeparators @ 0x5dff00 — strchr per separator @ 0x5dff4c, the shift @ 0x5dff73,
// the recursion @ 0x5dff90].
void strip_separators(std::string &token, char &removed) {
	for (const char separator : kSeparators) {
		const std::size_t at = token.find(separator);
		if (at == std::string::npos) continue;
		removed = token[at];
		token.erase(at, 1);
		if (token.find(separator) != std::string::npos) strip_separators(token, removed);
	}
}

// One line's tokens as a reader steps through them: the key (token 0), then each value through the
// readers' token helper [orig: CParticleDefEntry_NextToken @ 0x5dffb0]. The helper steps to the next
// slot, skips a slot of one separator alone (@ 0x5e0003..0x5e001e), strips the separators out of
// the token it takes, and a ';' the strip took ends the statement: the line's later calls answer
// none (`++dword_2C06570` @ 0x5e0044, the test @ 0x5dffce against the count the section callback
// set for the line @ 0x5ecb57). The tokenizer resets slots 0..2 to "" for every line, so the first two
// slots past a short line's tokens read "" [orig: Config_TokenizeLine @ 0x53cb71..0x53cb81]; a slot
// past those keeps an earlier line's token in retail, or none, which faults, and the port reads none
// there (D-PTL-33).
class LineTokens {
public:
	LineTokens(const io::ConfigTokens &tokens, const io::ConfigLineSpan &span, const std::string &text) {
		for (int i = 0; i < tokens.count; ++i) tokens_.emplace_back(tokens.tokens[i]);
		begin_.assign(span.token_begin, span.token_begin + tokens.count);
		end_.assign(span.token_end, span.token_end + tokens.count);
		cut_ = span.cut;
		line_begin_ = span.begin;
		line_end_ = span.end;
		text_ = &text;
	}

	const std::string &key() const { return tokens_[0]; }

	std::string *next() {
		for (;;) {
			if (ended_) return nullptr;
			++index_;
			std::string *token = nullptr;
			if (index_ < static_cast<int>(tokens_.size())) {
				token = &tokens_[static_cast<std::size_t>(index_)];
			} else if (index_ < 3) {
				reset_slot_.clear();
				token = &reset_slot_;
			} else {
				return nullptr;
			}
			if (token->size() == 1 && is_separator((*token)[0])) continue;
			char removed = '0';
			strip_separators(*token, removed);
			if (removed == ';') ended_ = true;
			return token;
		}
	}

	// Where the key's value is written: from the first token after the key that is not a separator
	// alone (and the commas before it the tokenizer dropped, as in `graphic1 =\t, blend;`) to the end
	// of the last token before the line's cut, the blanks and separators that end it left out; a key
	// of no value, the cut (the ';' that ends the line, else its end).
	void value_span(std::size_t &offset, std::size_t &length) const {
		std::size_t first = 1;
		while (first < tokens_.size() && tokens_[first].size() == 1 && is_separator(tokens_[first][0])) ++first;
		offset = std::min(cut_, line_end_);
		length = 0;
		if (first >= tokens_.size() || begin_[first] >= cut_) return;
		const std::string &text = *text_;
		std::size_t from = begin_[first];
		const auto blank = [&text](std::size_t at) { return text[at] == ' ' || text[at] == '\t'; };
		while (from > end_[first - 1] && (blank(from - 1) || text[from - 1] == ',')) --from;
		while (from < begin_[first] && blank(from)) ++from;
		std::size_t end = begin_[first];
		for (std::size_t i = first; i < tokens_.size(); ++i)
			if (begin_[i] < cut_) end = std::max(end, std::min(end_[i], cut_));
		while (end > begin_[first] && end <= text.size() &&
				(text[end - 1] == ' ' || text[end - 1] == '\t' || is_separator(text[end - 1])))
			--end;
		offset = from;
		length = end - from;
	}

	std::string value_text() const {
		std::size_t offset = 0, length = 0;
		value_span(offset, length);
		return text_->substr(offset, length);
	}

	std::size_t line_begin() const { return line_begin_; }

private:
	std::vector<std::string> tokens_;
	std::vector<std::size_t> begin_;
	std::vector<std::size_t> end_;
	std::size_t cut_ = 0;
	std::size_t line_begin_ = 0;
	std::size_t line_end_ = 0;
	const std::string *text_ = nullptr;
	std::string reset_slot_;
	int index_ = 0;
	bool ended_ = false;
};

// The CRT's atof and atol as the game links them (io::retail_atof: decimal only, a d exponent, no
// hex, infinity or NaN, whatever the process locale; io::retail_atol: 32 bits, saturating) [orig:
// CParticleDef_ParseProperties @ 0x5ea320, its _atof (@ 0x76B6A1) and j__atol (@ 0x76AB1B) calls].
float read_float(const std::string &token) {
	return static_cast<float>(io::retail_atof(token.c_str()));
}

std::int32_t read_long(const std::string &token) {
	return io::retail_atol(token.c_str());
}

// ------------------------------------------------------------ the game's defaults

// What an `id` line resets the particle scratch to: zeroed, then the game's defaults, the four graphic
// slots each taking the template values [orig: CParticleSystemDef_InitDefaults @ 0x5e14f0 — the memset
// @ 0x5e1500; the colours -1 @ 0x5e150d..0x5e151f; alpha 1.0 @ 0x5e1525 (+3304); age 3.0 @ 0x5e1537
// (+3716); emit_rate 2.0 @ 0x5e154a (+3624); the graphic count 1 @ 0x5e1550 (+3300); emit_burst 1
// @ 0x5e155c (+3708); emit_dur 10.0 @ 0x5e1562 (+3616); move 1, NORMAL, @ 0x5e1568 (+140); gravity
// 50.0 @ 0x5e157a (+3904); scale 1.0 @ 0x5e1588 (+3732); spread 10.0 @ 0x5e1590 (+3884); gravity_mask
// 1.0 x 3 @ 0x5e1596..0x5e15a2; each slot's colours, alpha, scale, scale_adj and curves from those,
// flip_frames 1 and flip_rate 8 @ 0x5e15a8..0x5e188c]. orbital_axis is not among them: 0, 0, 0.
ParticleDef particle_defaults() {
	ParticleDef def;
	def.emit_dur = 10.0f;
	def.emit_rate = 2.0f;
	def.emit_burst = 1;
	def.age = 3.0f;
	def.alpha = 1.0f;
	def.scale = 1.0f;
	def.color1 = def.color2 = def.color3 = def.color4 = Color3{255, 255, 255};
	def.spread = 10.0f;
	def.gravity = 50.0f;
	def.gravity_mask = {1.0f, 1.0f, 1.0f};
	def.move = move_flag::Normal;
	def.orbital_axis = {0.0f, 0.0f, 0.0f};
	for (std::size_t slot = 0; slot < def.graphics.size(); ++slot) {
		GraphicLayer &layer = def.graphics[slot];
		layer = GraphicLayer();
		layer.index = static_cast<int>(slot) + 1;
		layer.color1 = layer.color2 = layer.color3 = layer.color4 = def.color1;
		layer.alpha = def.alpha;
		layer.scale = def.scale;
		layer.scale_adj = def.scale_adj;
		layer.flip_frames = 1;
		layer.flip_rate = 8;
	}
	return def;
}

// ------------------------------------------------------------ the shared value reads

// A curve line over the entry it writes: the line's table id over the entry's, the id cut at a blank
// it holds (a quoted id), the words after that blank setting the mask (reverse 0x02, invert 0x01, by
// case-sensitive substring, as assigned) [orig: ParticleDef_ParsePlaybackFlags @ 0x5e2990, strstr
// @ 0x5e29b9 / @ 0x5e29d0], then the modifier after the id ORed into the entry's mask, so no line
// clears a bit an earlier line set (a repeated line; a graphic's line over the entry its graphicN
// line copied). That modifier is `reverse` (0x02) or `inverse` (0x01) without case, and no other
// word: `invert`, which NovaLogic's writers spell bit 0x01 (CurveRef_ModifierSuffix @ 0x42bf60;
// SoundBank_BuildFlagString @ 0x5e28d0), is none to the game, so `table2 invert` reads as `table2`.
// Retail reads that one token alone; the port reads every modifier after the id in either order,
// stopping at the first that is neither (D-PTL-20) [orig: CParticleDef_ParseProperties
// @ 0x5eb000..0x5eb07a (scale_func: the id copied over the entry, _stricmp "reverse" @ 0x5eb051 then
// `or dword_2C07468, 2`, _stricmp "inverse" @ 0x5eb06a then `or dword_2C07468, 1`), its alpha, emit
// rate, red, green and blue arms @ 0x5eb094..0x5eb413; CParticleDefEntry_ParseGraphicProperty
// @ 0x5e3b0e..0x5e3bd3 and its siblings]. An `id` line alone clears an entry (the scratch reset).
void read_curve(LineTokens &line, CurveRef &entry) {
	const std::string *id = line.next();
	if (!id) return;
	entry.present = true;
	entry.name = *id;
	const std::size_t blank = entry.name.find(' ');
	if (blank != std::string::npos) {
		const std::string words = entry.name.substr(blank + 1);
		entry.name.resize(blank);
		entry.reverse = words.find("reverse") != std::string::npos;
		entry.inverse = words.find("invert") != std::string::npos;
	}
	while (const std::string *modifier = line.next()) {
		const std::string word = strutil::to_lower(*modifier);
		if (word == "reverse") {
			entry.reverse = true;
		} else if (word == "inverse") {
			entry.inverse = true;
		} else {
			break;
		}
	}
}

// A colour line: up to three whole numbers into the colour channels every colour line shares, red,
// green, then blue, each read only after the one before it, and the colour packed from the three as
// they stand, each its low byte: a channel a line leaves unwritten keeps what an earlier colour line,
// in any block or file, wrote [orig: CParticleDef_ParseProperties @ 0x5eac16..0x5eacb2 (color1: atol
// into dword_2C06560, dword_2C06568, dword_2C06564, packed HIBYTE/LOBYTE @ 0x5eaca2..0x5eacb2), the
// other three @ 0x5eacca..0x5eaed2; CParticleDefEntry_ParseGraphicProperty @ 0x5e37c3..0x5e3aa7].
void read_color(LineTokens &line, ParticleReadState &state, Color3 &out) {
	if (const std::string *red = line.next()) {
		state.red = read_long(*red);
		if (const std::string *green = line.next()) {
			state.green = read_long(*green);
			if (const std::string *blue = line.next()) state.blue = read_long(*blue);
		}
	}
	out.r = static_cast<std::uint8_t>(state.red & 0xFF);
	out.g = static_cast<std::uint8_t>(state.green & 0xFF);
	out.b = static_cast<std::uint8_t>(state.blue & 0xFF);
}

// Three numbers, each read only after the one before it [orig: e.g. emit_shape_size
// @ 0x5ea9d9..0x5eaa62].
void read_vec3(LineTokens &line, Vec3 &out) {
	if (const std::string *x = line.next()) {
		out.x = read_float(*x);
		if (const std::string *y = line.next()) {
			out.y = read_float(*y);
			if (const std::string *z = line.next()) out.z = read_float(*z);
		}
	}
}

void read_real(LineTokens &line, float &out) {
	if (const std::string *value = line.next()) out = read_float(*value);
}

void read_whole(LineTokens &line, int &out) {
	if (const std::string *value = line.next()) out = read_long(*value);
}

void read_text(LineTokens &line, std::string &out) {
	if (const std::string *value = line.next()) out = *value;
}

// A flags or move list: each token that names an entry of the table, whole and without case, joined
// with a blank after each, up to the first that names none; then the bits of every entry whose name
// is in that text, by substring and with case [orig: CParticleDef_ParseProperties @ 0x5ea49f..0x5ea56f
// (move) and @ 0x5ea59f..0x5ea66f (flags); FlagTable_ParseFromString @ 0x5df970, strstr @ 0x5df995].
template <typename IsName, typename Parse>
std::uint32_t read_flag_list(LineTokens &line, IsName is_name, Parse parse) {
	std::string text;
	while (const std::string *token = line.next()) {
		if (!is_name(*token)) break;
		text += *token;
		text += ' ';
	}
	return parse(text);
}

// ------------------------------------------------------------ the readers

struct Reader {
	ParticleFile &out;
	ParticleReadState &state;
	ParticlePlaces *places = nullptr;
	const std::string &text;
	int line_number = 0;
	int header_line = 0; // the last section header's line in this text
	bool effect_block = false; // between an effect section's `{` and `}`, for the places alone
	std::size_t effect_block_keys = 0; // the places' key count at that `{`

	// ---- places

	void place_key(BlockKind kind, std::size_t index, const LineTokens &line) {
		if (!places) return;
		KeyPlace place;
		place.block = kind;
		place.index = index;
		place.key = line.key();
		place.line = line_number;
		line.value_span(place.offset, place.length);
		places->keys.push_back(std::move(place));
	}

	void place_block(BlockKind kind, std::size_t index, int first_line, const LineTokens &line) {
		if (!places) return;
		BlockPlace place;
		place.kind = kind;
		place.index = index;
		place.first_line = first_line;
		place.last_line = line_number;
		place.close_offset = line.line_begin();
		places->blocks.push_back(place);
	}

	// ---- the section callback [orig: CEffectWorld_ParseSectionCallback @ 0x5ecb40]

	// One line the walk hands the callback; true ends the walk.
	bool section_line(LineTokens &line) {
		const std::string &key = line.key();
		if (strutil::iequals(key, "[effectdef]")) {
			state.section = Section::Effect; // @ 0x5ecb66..0x5ecb80
			header_line = line_number;
			return false;
		}
		if (strutil::iequals(key, "[particledef]")) {
			state.section = Section::Particle; // @ 0x5ecb92..0x5ecbac
			header_line = line_number;
			return false;
		}
		if (strutil::iequals(key, "[tabledef]")) {
			state.section = Section::Table; // @ 0x5ecbbe..0x5ecbd8
			header_line = line_number;
			return false;
		}
		if (strutil::iequals(key, "[tabledef_edithandles]")) return true; // @ 0x5ecbea
		switch (state.section) {
			case Section::Effect: return effect_line(line);     // @ 0x5ecc0b
			case Section::Particle: return particle_line(line); // @ 0x5ecc16
			case Section::Table: return table_line(line);       // @ 0x5ecc2c
			case Section::None: break;
		}
		return true; // a line before any section header ends the walk (@ 0x5ecc3a)
	}

	// ---- [effectdef] [orig: CParticleTableDef_ParseScriptLine @ 0x5e92b0]

	bool effect_line(LineTokens &line) {
		const std::string &key = line.key();
		if (key.find("[tabledef_edithandles]") != std::string::npos) return true; // strstr @ 0x5e92ea
		if (strutil::iequals(key, "{")) {
			effect_block = true; // nothing to the game @ 0x5e9325; the block's keys' places from here
			effect_block_keys = places ? places->keys.size() : 0;
			return false;
		}
		const std::size_t index = out.effects.size();
		if (!strutil::iequals(key, "}") && (effect_block || state.effect_open)) place_key(BlockKind::Effect, index, line);
		if (strutil::iequals(key, "id")) {
			// The name copied into the one name buffer, which outlives its block @ 0x5e9355..0x5e9386.
			if (const std::string *name = line.next()) {
				state.effect_name = *name;
				std::size_t offset = 0, length = 0;
				line.value_span(offset, length);
				state.effect_name_line = line_number;
				state.effect_name_column = static_cast<int>(offset - line.line_begin()) + 1;
			}
			return false;
		}
		if (!strutil::iequals(key, "pdefs")) {
			// `}` registers the effect a pdefs line built @ 0x5e94e4..0x5e94f8; any other key is none.
			if (strutil::iequals(key, "}") && effect_block) {
				// A block that built no effect (no pdefs line) is nothing the game holds: its keys have no
				// effect to be placed in.
				if (!state.effect_open && places) {
					std::vector<KeyPlace> &keys = places->keys;
					const auto from = keys.begin() + static_cast<std::ptrdiff_t>(std::min(effect_block_keys, keys.size()));
					const auto in_block = [index](const KeyPlace &place) {
						return place.block == BlockKind::Effect && place.index == index;
					};
					keys.erase(std::remove_if(from, keys.end(), in_block), keys.end());
				}
				effect_block = false;
			}
			if (strutil::iequals(key, "}") && state.effect_open) {
				EffectDef effect = std::move(state.effect);
				effect.last_line = line_number;
				place_block(BlockKind::Effect, index, effect.first_line, line);
				out.effects.push_back(std::move(effect));
				state.effect = EffectDef();
				state.effect_open = false;
			}
			return false;
		}
		// pdefs: a new effect of the name buffer's name, every token of the line its member
		// @ 0x5e93cb..0x5e9498 (the one an earlier pdefs line built and no `}` registered is dropped).
		state.effect = EffectDef();
		state.effect.id = state.effect_name;
		state.effect.first_line = header_line;
		state.effect.id_line = state.effect_name_line;
		state.effect.id_column = state.effect_name_column;
		while (const std::string *member = line.next()) state.effect.pdefs.push_back(*member);
		state.effect_open = true;
		return false;
	}

	// ---- [particledef] [orig: CParticleDef_ParseProperties @ 0x5ea320]

	void graphic_decl(LineTokens &line, bool first) {
		// [orig: CParticleDefEntry_ParseGraphicProperty @ 0x5e3568..0x5e3702] graphic1 takes the
		// first layer and a count of one, any other graphicN the next layer (up to the fourth) and
		// one more (up to four) @ 0x5e357e..0x5e35b9; the texture and the blend mode @ 0x5e35d7..
		// 0x5e362f; the particle's colours, alpha, scale, scale_adj and five curve entries, names and
		// masks, as they stand at this line @ 0x5e3635..0x5e36ec; every layer's flip_frames back to 1
		// @ 0x5e36f0..0x5e3702. The config-map hydrator's copy, which took the particle's values after
		// all of its keys [orig: CParticleDef_ParseFromConfigMap @ 0x5ee9d0..0x5eea3d], is on no load's
		// path (its one caller, CEffectWorld_LoadAllDefs @ 0x5ef1f0, is called only by
		// CEffectWorld_LoadFromFile @ 0x5ef800, which nothing references; D-PTL-38).
		if (first) {
			state.graphic_index = 0;
			state.graphic_count = 1;
		} else {
			if (state.graphic_index < 3) ++state.graphic_index;
			if (state.graphic_count < 4) ++state.graphic_count;
		}
		ParticleDef &def = state.scratch;
		GraphicLayer &layer = def.graphics[static_cast<std::size_t>(state.graphic_index)];
		read_text(line, layer.texture);
		if (const std::string *blend = line.next()) layer.blend_mode = parse_blend_mode(*blend);
		layer.color1 = def.color1;
		layer.color2 = def.color2;
		layer.color3 = def.color3;
		layer.color4 = def.color4;
		layer.alpha = def.alpha;
		layer.scale = def.scale;
		layer.scale_adj = def.scale_adj;
		layer.scale_func = def.scale_func;
		layer.alpha_func = def.alpha_func;
		layer.red_func = def.red_func;
		layer.green_func = def.green_func;
		layer.blue_func = def.blue_func;
		for (GraphicLayer &each : def.graphics) each.flip_frames = 1;
	}

	// A layer's key, on the layer the last graphic line took whatever its N [orig:
	// CParticleDefEntry_ParseGraphicProperty @ 0x5e3713..0x5e4001, every field at `788 * dword_2C0654C`].
	// The colours keep their own slot (D-PTL-1).
	bool graphic_field(LineTokens &line, const std::string &field) {
		GraphicLayer &layer = state.scratch.graphics[static_cast<std::size_t>(state.graphic_index)];
		if (field == "flip_frames") {
			// atol's value as it is in retail; the port's bound 1..kMaxParticleFlipFrames (D-PTL-19).
			if (const std::string *value = line.next())
				layer.flip_frames = std::clamp(static_cast<int>(read_long(*value)), 1, kMaxParticleFlipFrames);
		} else if (field == "flip_rate") {
			read_whole(line, layer.flip_rate);
		} else if (field == "color1") {
			read_color(line, state, layer.color1);
		} else if (field == "color2") {
			read_color(line, state, layer.color2);
		} else if (field == "color3") {
			read_color(line, state, layer.color3);
		} else if (field == "color4") {
			read_color(line, state, layer.color4);
		} else if (field == "alpha") {
			read_real(line, layer.alpha);
		} else if (field == "scale") {
			read_real(line, layer.scale);
		} else if (field == "scale_adj") {
			read_real(line, layer.scale_adj);
		} else if (field == "scale_func") {
			read_curve(line, layer.scale_func);
		} else if (field == "alpha_func") {
			read_curve(line, layer.alpha_func);
		} else if (field == "red_func") {
			read_curve(line, layer.red_func);
		} else if (field == "green_func") {
			read_curve(line, layer.green_func);
		} else if (field == "blue_func") {
			read_curve(line, layer.blue_func);
		} else {
			return false;
		}
		return true;
	}

	// The layer key a gN_ key names, the dispatcher's own list for each N [orig:
	// CParticleDef_ParseProperties @ 0x5ebc34..0x5ec621]; the dispatcher and the original editor's reader
	// [orig: ParticleDef_ParseField @ 0x433e20 (ParticleEdit_v1_1.exe)] remap some colour keys, which the
	// port maps 1:1 (D-PTL-1).
	static bool graphic_key(const std::string &key, std::string &field) {
		static const char *const kFields[] = {"flip_frames", "flip_rate", "color1", "color2", "color3",
				"color4", "alpha", "scale", "scale_adj", "scale_func", "alpha_func", "red_func", "green_func",
				"blue_func"};
		if (key.size() < 4 || (key[0] != 'g' && key[0] != 'G') || key[1] < '1' || key[1] > '4' || key[2] != '_')
			return false;
		const std::string rest = strutil::to_lower(std::string_view(key).substr(3));
		for (const char *each : kFields) {
			if (rest == each) {
				field = rest;
				return true;
			}
		}
		return false;
	}

	bool particle_line(LineTokens &line) {
		const std::string &key = line.key();
		ParticleDef &def = state.scratch;
		if (key.find("[tabledef_edithandles]") != std::string::npos) return true; // strstr @ 0x5ea34c
		if (strutil::iequals(key, "{")) {
			state.graphic_count = 0; // @ 0x5ea38c
			return false;
		}
		const std::size_t index = out.particles.size();
		if (strutil::iequals(key, "id")) {
			// A new definition, the scratch at the game's defaults @ 0x5ea3ca..0x5ea41b (one an earlier
			// id opened and no `}` registered is dropped).
			if (const std::string *id = line.next()) {
				def = particle_defaults();
				state.graphic_count = 1;
				def.id = *id;
				state.particle_open = true;
			}
			place_key(BlockKind::Particle, index, line);
			return false;
		}
		if (strutil::iequals(key, "}")) {
			// The scratch copied into the definition the last id opened and registered
			// @ 0x5ec7e1..0x5ec824; a `}` with none open faults in retail (the copy into null
			// @ 0x5ec7f8), the port registers nothing.
			if (state.particle_open) {
				ParticleDef committed = def;
				for (std::size_t slot = 0; slot < committed.graphics.size(); ++slot) {
					committed.graphics[slot].present = static_cast<int>(slot) < state.graphic_count;
					committed.graphics[slot].index = static_cast<int>(slot) + 1;
				}
				place_block(BlockKind::Particle, index, header_line, line);
				out.particles.push_back(std::move(committed));
				state.particle_open = false;
			}
			return false;
		}
		if (state.particle_open) place_key(BlockKind::Particle, index, line);
		if (strutil::iequals(key, "child_id")) {
			read_text(line, def.child_id); // @ 0x5ea43f..0x5ea481
		} else if (strutil::iequals(key, "move")) {
			def.move = read_flag_list(line, is_move_flag_name, parse_move_bits);
		} else if (strutil::iequals(key, "flags")) {
			def.flags = read_flag_list(line, is_particle_flag_name, parse_particle_flags);
		} else if (strutil::iequals(key, "emit_dur")) {
			read_real(line, def.emit_dur);
		} else if (strutil::iequals(key, "emit_dur_adj")) {
			read_real(line, def.emit_dur_adj);
		} else if (strutil::iequals(key, "emit_rate")) {
			read_real(line, def.emit_rate);
		} else if (strutil::iequals(key, "emit_rate_adj")) {
			read_real(line, def.emit_rate_adj);
		} else if (strutil::iequals(key, "emit_delay")) {
			read_real(line, def.emit_delay);
		} else if (strutil::iequals(key, "emit_burst")) {
			// A burst under 1 is 1 @ 0x5ea86f..0x5ea87a.
			if (const std::string *value = line.next()) def.emit_burst = std::max(1, static_cast<int>(read_long(*value)));
		} else if (strutil::iequals(key, "emit_maxoverride")) {
			read_whole(line, def.emit_maxoverride);
		} else if (strutil::iequals(key, "y_offset")) {
			read_real(line, def.y_offset);
		} else if (strutil::iequals(key, "z_offset")) {
			read_real(line, def.z_offset);
		} else if (strutil::iequals(key, "emit_shape")) {
			read_whole(line, def.emit_shape);
		} else if (strutil::iequals(key, "emit_shape_size")) {
			read_vec3(line, def.emit_shape_size);
		} else if (strutil::iequals(key, "emit_shape_size_skip")) {
			read_vec3(line, def.emit_shape_size_skip);
		} else if (strutil::iequals(key, "age")) {
			read_real(line, def.age); // off_7DD900 @ 0x5eab23
		} else if (strutil::iequals(key, "age_adj")) {
			read_real(line, def.age_adj);
		} else if (strutil::iequals(key, "alpha")) {
			read_real(line, def.alpha);
		} else if (strutil::iequals(key, "color1")) {
			read_color(line, state, def.color1);
		} else if (strutil::iequals(key, "color2")) {
			read_color(line, state, def.color2);
		} else if (strutil::iequals(key, "color3")) {
			read_color(line, state, def.color3);
		} else if (strutil::iequals(key, "color4")) {
			read_color(line, state, def.color4);
		} else if (strutil::iequals(key, "bump_scale")) {
			read_real(line, def.bump_scale);
		} else if (strutil::iequals(key, "scale")) {
			read_real(line, def.scale);
		} else if (strutil::iequals(key, "scale_adj")) {
			read_real(line, def.scale_adj);
		} else if (strutil::iequals(key, "scale_func")) {
			read_curve(line, def.scale_func);
		} else if (strutil::iequals(key, "alpha_func")) {
			read_curve(line, def.alpha_func);
		} else if (strutil::iequals(key, "emit_rate_func")) {
			read_curve(line, def.emit_rate_func);
		} else if (strutil::iequals(key, "red_func")) {
			read_curve(line, def.red_func);
		} else if (strutil::iequals(key, "green_func")) {
			read_curve(line, def.green_func);
		} else if (strutil::iequals(key, "blue_func")) {
			read_curve(line, def.blue_func);
		} else if (strutil::iequals(key, "orientation")) {
			read_vec3(line, def.orientation);
		} else if (strutil::iequals(key, "orientationadj")) {
			read_vec3(line, def.orientationadj);
		} else if (strutil::iequals(key, "yaw_rot")) {
			read_real(line, def.yaw_rot);
		} else if (strutil::iequals(key, "yaw_rot_adj")) {
			read_real(line, def.yaw_rot_adj);
		} else if (strutil::iequals(key, "pitch_rot")) {
			read_real(line, def.pitch_rot);
		} else if (strutil::iequals(key, "pitch_rot_adj")) {
			read_real(line, def.pitch_rot_adj);
		} else if (strutil::iequals(key, "roll_rot")) {
			read_real(line, def.roll_rot);
		} else if (strutil::iequals(key, "roll_rot_adj")) {
			read_real(line, def.roll_rot_adj);
		} else if (strutil::iequals(key, "speed")) {
			read_real(line, def.speed);
		} else if (strutil::iequals(key, "speed_adj")) {
			read_real(line, def.speed_adj);
		} else if (strutil::iequals(key, "elastic")) {
			read_real(line, def.elastic);
		} else if (strutil::iequals(key, "gravity")) {
			read_real(line, def.gravity);
		} else if (strutil::iequals(key, "gravity_mask")) {
			read_vec3(line, def.gravity_mask);
		} else if (strutil::iequals(key, "drag")) {
			read_real(line, def.drag);
		} else if (strutil::iequals(key, "spread")) {
			read_real(line, def.spread);
		} else if (strutil::iequals(key, "spread_skip")) {
			read_real(line, def.spread_skip);
		} else if (strutil::iequals(key, "orbitalspeed")) {
			read_real(line, def.orbitalspeed);
		} else if (strutil::iequals(key, "orbitalspeed_adj")) {
			read_real(line, def.orbitalspeed_adj);
		} else if (strutil::iequals(key, "orbital_axis")) {
			read_vec3(line, def.orbital_axis);
		} else if (strutil::starts_with_icase(key, "collide_sound")) {
			// The slot is atol of what follows the 13-byte prefix, taken under 20 unsigned
			// @ 0x5ebb82..0x5ebba7; one past them the game reads nothing of, and the port keeps the
			// line for the writer, as a key of no case (ParticleDef::unknown_keys).
			const std::uint32_t slot = static_cast<std::uint32_t>(io::retail_atol(key.c_str() + 13));
			if (slot < def.collide_sounds.size()) {
				read_text(line, def.collide_sounds[slot]);
			} else {
				def.unknown_keys.emplace_back(key, line.value_text());
			}
		} else if (strutil::iequals(key, "graphic1")) {
			graphic_decl(line, true);
		} else if (strutil::iequals(key, "graphic2") || strutil::iequals(key, "graphic3") ||
				strutil::iequals(key, "graphic4")) {
			graphic_decl(line, false);
		} else {
			std::string field;
			if (graphic_key(key, field) && graphic_field(line, field)) return false;
			// A key the reader has no case for. The game reads nothing of it; the port keeps the
			// writer's lod and the line for the writer (ParticleDef::unknown_keys).
			if (strutil::iequals(key, "lod")) {
				read_real(line, def.lod);
			} else {
				def.unknown_keys.emplace_back(key, line.value_text());
			}
		}
		return false;
	}

	// ---- [tabledef] [orig: CEffectTableDef_ParseCallback @ 0x5e4010]

	// One byte through the row cursor: into the table the last `{` opened, which keeps it once
	// registered. A table is 0x250 bytes with its 256-byte LUT at +0x148, so the cursor's bytes 256..259
	// are the table's transform-flags dword (+0x248), 260..263 its manager pointer (+0x24C) and on from
	// 264 past the allocation; the port writes the LUT and the flags and drops the rest, as it drops a
	// byte through a cursor no `{` set yet (a write through null in retail) or one into an earlier file's
	// table: one file's lines never change another file's table (D-PTL-39).
	void write_row_byte(std::uint8_t byte) {
		TableDef *table = nullptr;
		if (state.cursor == Cursor::Open) {
			table = &state.table;
		} else if (state.cursor == Cursor::Registered && state.cursor_table < out.tables.size()) {
			table = &out.tables[state.cursor_table];
		}
		const std::size_t at = state.cursor_offset++;
		if (!table) return;
		if (at < 256) {
			table->rows[at / 8][at % 8] = byte;
		} else if (at < 260) {
			const unsigned shift = static_cast<unsigned>(at - 256) * 8u;
			table->flags = (table->flags & ~(0xFFu << shift)) | (static_cast<std::uint32_t>(byte) << shift);
		}
	}

	bool table_line(LineTokens &line) {
		const std::string &key = line.key();
		if (key.find("[tabledef_edithandles]") != std::string::npos) return true; // strstr @ 0x5e4026
		if (strutil::iequals(key, "{")) {
			// A new table, its flags 0, the cursor at its LUT @ 0x5e4058..0x5e40a9 (one an earlier `{`
			// opened and no `}` registered is dropped).
			state.table = TableDef();
			state.table.rows.assign(32, std::array<std::uint8_t, 8>{});
			state.table_open = true;
			state.cursor = Cursor::Open;
			state.cursor_offset = 0;
			return false;
		}
		const std::size_t index =
				!state.table_open && state.cursor == Cursor::Registered ? state.cursor_table : out.tables.size();
		if (!strutil::iequals(key, "}") && (state.table_open || state.cursor == Cursor::Registered))
			place_key(BlockKind::Table, index, line);
		if (strutil::iequals(key, "id")) {
			// The row count reset, then the name @ 0x5e40c2..0x5e4100; an id with no table open names
			// none (a call through null in retail).
			state.row_count = 0;
			if (const std::string *name = line.next()) {
				if (state.table_open) state.table.id = *name;
			}
			return false;
		}
		if (key.find('t') == std::string::npos) {
			// `}` registers the open table @ 0x5e41a1..0x5e41b5; any other key is none.
			if (strutil::iequals(key, "}") && state.table_open) {
				place_block(BlockKind::Table, out.tables.size(), header_line, line);
				out.tables.push_back(std::move(state.table));
				state.table = TableDef();
				state.table_open = false;
				if (state.cursor == Cursor::Open) {
					state.cursor = Cursor::Registered;
					state.cursor_table = out.tables.size() - 1;
				}
			}
			return false;
		}
		// Any other key holding a 't' is the next row: up to 8 bytes through atol while fewer than 32
		// rows were read since the last id, then one more row counted @ 0x5e4120..0x5e4171.
		for (int column = 0; column < 8; ++column) {
			if (state.row_count >= 32) break;
			const std::string *value = line.next();
			if (!value) break;
			write_row_byte(static_cast<std::uint8_t>(read_long(*value) & 0xFF));
		}
		++state.row_count;
		return false;
	}

	// ---- the original editor's [tabledef_edithandles] blocks, which the game never reads

	struct HandlesPass {
		bool open = false;
		bool collecting = false;
		TableEditHandles handles;
		int first_line = 0;
	};

	void handles_line(HandlesPass &pass, LineTokens &line) {
		const std::string &key = line.key();
		const std::size_t index = out.table_handles.size();
		if (strutil::iequals(key, "[tabledef_edithandles]")) {
			if (pass.open) out.table_handles.push_back(std::move(pass.handles));
			pass = HandlesPass();
			pass.open = true;
			pass.collecting = true;
			pass.first_line = line_number;
			return;
		}
		if (strutil::iequals(key, "[effectdef]") || strutil::iequals(key, "[particledef]") ||
				strutil::iequals(key, "[tabledef]")) {
			if (pass.open) out.table_handles.push_back(std::move(pass.handles));
			pass = HandlesPass();
			return;
		}
		if (!pass.collecting) return;
		if (key[0] == '}') {
			if (pass.open) {
				place_block(BlockKind::Handles, index, pass.first_line, line);
				out.table_handles.push_back(std::move(pass.handles));
			}
			pass = HandlesPass();
			return;
		}
		if (key == "{" || !pass.open) return;
		place_key(BlockKind::Handles, index, line);
		if (strutil::iequals(key, "tableid")) {
			read_text(line, pass.handles.table_id);
		} else if (strutil::iequals(key, "handlecount")) {
			read_whole(line, pass.handles.handlecount);
		} else if (strutil::iequals(key, "tightness")) {
			read_whole(line, pass.handles.tightness);
		}
	}
};

bool read_particles(const char *data, std::size_t size, ParticleFile &out, ParticlePlaces *places,
		ParticleReadState &state) {
	out = ParticleFile();
	if (places) *places = ParticlePlaces();
	// What an earlier file left that names a place in it: its effect name's line, its registered
	// table's cursor (whose writes the port drops from here on).
	state.effect_name_line = 0;
	state.effect_name_column = 0;
	if (state.cursor == Cursor::Registered) state.cursor = Cursor::Elsewhere;
	const std::string text(data != nullptr ? data : "", data != nullptr ? size : 0);
	Reader reader{out, state, places, text};
	io::ConfigTokens tokens;
	bool ended = false;
	int ended_line = 0;
	io::for_each_config_line_span(text.data(), text.size(), tokens,
			[&](io::ConfigTokens &line_tokens, const io::ConfigLineSpan &span) -> bool {
				++reader.line_number;
				// The walk's gate: a line with no token, or whose first starts with '/', never reaches
				// the callback [orig: File_ParseASCIIFile @ 0x53d915 / @ 0x53d91e].
				if (line_tokens.count == 0 || line_tokens.tokens[0][0] == '/') return false;
				LineTokens line(line_tokens, span, text);
				if (!reader.section_line(line)) return false;
				ended = true;
				ended_line = reader.line_number;
				return true; // a nonzero callback ends File_ParseASCIIFile's loop [orig: @ 0x53d942]
			});
	if (!ended) return true;
	// From the line that ended the walk on, the original editor's handle blocks alone.
	Reader::HandlesPass pass;
	reader.line_number = 0;
	io::ConfigTokens rest;
	io::for_each_config_line_span(text.data(), text.size(), rest,
			[&](io::ConfigTokens &line_tokens, const io::ConfigLineSpan &span) -> bool {
				++reader.line_number;
				if (reader.line_number < ended_line) return false;
				if (line_tokens.count == 0 || line_tokens.tokens[0][0] == '/') return false;
				LineTokens line(line_tokens, span, text);
				reader.handles_line(pass, line);
				return false;
			});
	if (pass.open) out.table_handles.push_back(std::move(pass.handles));
	return true;
}

} // namespace

bool load_particles(std::istream &input, ParticleFile &out, ParseError &error) {
	error = ParseError();
	const std::string buffer{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
	ParticleReadState state;
	return read_particles(buffer.data(), buffer.size(), out, nullptr, state);
}

bool load_particles_from_buffer(const char *data, std::size_t size, ParticleFile &out, ParseError &error) {
	ParticleReadState state;
	return load_particles_from_buffer(data, size, out, state, error);
}

bool load_particles_from_buffer(const char *data, std::size_t size, ParticleFile &out, ParticleReadState &state,
                                ParseError &error) {
	error = ParseError();
	return read_particles(data, size, out, nullptr, state);
}

bool load_particles_with_places(const char *data, std::size_t size, ParticleFile &out, ParticlePlaces &places,
                                ParseError &error) {
	ParticleReadState state;
	return load_particles_with_places(data, size, out, places, state, error);
}

bool load_particles_with_places(const char *data, std::size_t size, ParticleFile &out, ParticlePlaces &places,
                                ParticleReadState &state, ParseError &error) {
	error = ParseError();
	return read_particles(data, size, out, &places, state);
}

bool load_particles_from_file(const std::string &path, ParticleFile &out, ParseError &error) {
	std::ifstream stream(io::os_path(path), std::ios::binary); // UTF-8, long-path safe
	if (!stream) {
		error = ParseError();
		error.message = "Cannot open " + path;
		out = ParticleFile();
		return false;
	}
	return load_particles(stream, out, error);
}

} // namespace opennova::particle
