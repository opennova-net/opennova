// The sound bank document (sound_bank_document.h): the bank's table over its native records, its
// parse through the engine's reader (formats/lwf), its save through the engine's writer from the
// rows alone, and its findings. The fields' words say what the game does with each value, each
// witnessed in docs/audio/lwf-dbf-sound-re.md.
#include "sound_bank_document.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/project/project_files.h>

namespace opennova::editor {
namespace {

constexpr NodeKind kWave = node_kind(SoundBankKind::Wave);
constexpr NodeKind kSet = node_kind(SoundBankKind::Set);
constexpr NodeKind kLayer = node_kind(SoundBankKind::Layer);
constexpr NodeKind kMember = node_kind(SoundBankKind::Member);

// The bytes the format keeps a name in, its terminator among them: a wave's 32 [orig: the 52-byte
// entry, the name at +0, stricmp'd whole by SoundBank_FindEntryByName @ 0x75bba0], a set's 24 [orig:
// SoundBank_LoadTriggerSets @ 0x75c43e..0x75c461 copies six dwords of name; the in-memory record's
// pitch follows at +28], a wave's file 256 [orig: the 256-byte filename slots, @ 0x75c688].
constexpr size_t kWaveNameBytes = 32;
constexpr size_t kSetNameBytes = 24;
constexpr size_t kWaveFileBytes = 256;

const BankWave &wave_of(const RecordHandle &r) { return r.as<BankWave>(); }
const BankSet &set_of(const RecordHandle &r) { return r.as<BankSet>(); }
const BankLayer &layer_of(const RecordHandle &r) { return r.as<BankLayer>(); }
const BankMember &member_of(const RecordHandle &r) { return r.as<BankMember>(); }

// A number a field takes, whole or not (the wire and the widgets give either).
bool number_of(const Value &value, double &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = double(*whole);
		return true;
	}
	if (const auto *real = std::get_if<double>(&value)) {
		out = *real;
		return std::isfinite(out);
	}
	return false;
}

bool whole_of(const Value &value, int64_t &out) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = *whole;
		return true;
	}
	if (const auto *real = std::get_if<double>(&value); real && std::isfinite(*real) && *real == std::floor(*real)) {
		out = int64_t(*real);
		return true;
	}
	return false;
}

// A name the game looks up: plain printable ASCII within the bytes its field keeps.
bool set_name(std::string &field, size_t bytes, const char *what, const Value &value, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) {
		error = std::string(what) + " is a text.";
		return false;
	}
	for (const unsigned char c : *text)
		if (c < 0x20 || c > 0x7E) {
			error = std::string(what) + " is plain ASCII: the game compares it byte for byte.";
			return false;
		}
	if (text->size() >= bytes) {
		error = std::string(what) + " holds at most " + std::to_string(bytes - 1) + " characters: the file keeps " +
		        std::to_string(bytes) + " bytes for it, its terminator among them.";
		return false;
	}
	field = *text;
	return true;
}

// A Q16 word shown as the ratio it is (0x10000 = 1).
Value ratio_value(uint32_t q16) { return double(q16) / double(lwf::kPitchUnityQ16); }

bool set_ratio(uint32_t &field, const Value &value, const char *what, std::string &error) {
	double number = 0.0;
	if (!number_of(value, number) || number < 0.0 || number * lwf::kPitchUnityQ16 > double(INT32_MAX)) {
		error = std::string(what) + " is a number from 0 to 32767.";
		return false;
	}
	field = uint32_t(std::lround(number * lwf::kPitchUnityQ16));
	return true;
}

bool set_whole(uint32_t &field, const Value &value, int64_t max, const char *what, std::string &error) {
	int64_t number = 0;
	if (!whole_of(value, number) || number < 0 || number > max) {
		error = std::string(what) + " is a whole number from 0 to " + std::to_string(max) + ".";
		return false;
	}
	field = uint32_t(number);
	return true;
}

FieldSchema schema_of(const char *id, FieldType type, const char *label, const char *description) {
	FieldSchema schema;
	schema.id = id;
	schema.type = type;
	schema.label = label;
	schema.description = description;
	return schema;
}

FieldSchema ranged(FieldSchema schema, double min, double max, double step = 0.0) {
	schema.ranged = true;
	schema.min = min;
	schema.max = max;
	schema.step = step;
	return schema;
}

// The layer flags by what each does; the selection pair and the two views witnessed, the rest the
// authoring tools' (formats/lwf PlaylistFlags).
const std::vector<FieldChoice> &layer_flag_choices() {
	static const std::vector<FieldChoice> choices = {
		{"heading", lwf::kFlagHeading, "Pans from the emitter's heading"},
		{"internal", lwf::kFlagInternal, "Heard in the first-person view"},
		{"external", lwf::kFlagExternal, "Heard in the outside view"},
		{"random", lwf::kFlagRandom, "Random (the tools' mark: random is what the game does unmarked)"},
		{"sequential", lwf::kFlagSequential, "Members in order"},
		{"view_bit", lwf::kFlagStoppable, "View bit (matched where the set gates by view)"},
		{"preload", lwf::kFlagPreload, "Loads its waves with the bank"},
		{"random_sequential", lwf::kFlagRandomSequential, "A random start, then in order"},
		{"directional", lwf::kFlagDirectional, "Directional (the tools' flag)"},
		{"looping", lwf::kFlagLooping, "Looping (the tools' flag)"},
		{"reverb", lwf::kFlagReverb, "Reverb (the tools' flag)"},
		{"rapid", lwf::kFlagRapid, "Rapid (the tools' flag)"},
	};
	return choices;
}

RecordTable make_table() {
	using RF = LabelledField;
	// --- a wave ---------------------------------------------------------------------------------
	TableKind wave(RecordKindRow{kWave, "wave", "Wave", "Add wave", true});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Name",
				"What the bank's members, and a mission's dialog lines, name this wave by; the bank finds the "
				"first wave of a name, without case [orig: SoundBank_FindEntryByName @ 0x75bba0].");
		name.width = kWaveNameBytes;
		name.defines = ReferenceKind::BankWave;
		wave.field(RF{name,
		              {[](const RecordHandle &r, Value &out) { return out = wave_of(r).name, true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_name(r.as<BankWave>().name, kWaveNameBytes, "A wave's name", v, e);
		               }}});
		FieldSchema file = schema_of("file", FieldType::Text, "File",
				"The .wav the game loads for this wave, by its name, from the archives the first time a member "
				"plays it [orig: sub_75BC20 @ 0x75bc20].");
		file.width = kWaveFileBytes;
		file.reference = ReferenceKind::Wave;
		wave.field(RF{file,
		              {[](const RecordHandle &r, Value &out) { return out = wave_of(r).file, true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_name(r.as<BankWave>().file, kWaveFileBytes, "A wave's file", v, e);
		               }}});
		wave.field(RF{ranged(schema_of("volume", FieldType::Integer, "Dialog volume",
		                               "The volume, 0 to 255, a mission's dialog line plays this wave at when it names "
		                               "it, 0 playing at full [orig: Dialog_LoadAudioClip @ 0x44dd10, the entry's byte +33 "
		                               "through sub_75BE10 @ 0x75be10; the play hook sub_527560 @ 0x527598]; a set's "
		                               "members carry their own."),
		                     0, 255, 1),
		              {[](const RecordHandle &r, Value &out) { return out = int64_t(wave_of(r).volume), true; },
		               [](const RecordHandle &r, const Value &v, std::string &e) {
			               return set_whole(r.as<BankWave>().volume, v, 255, "A volume", e);
		               }}});
	}
	// --- a set ----------------------------------------------------------------------------------
	TableKind set(RecordKindRow{kSet, "set", "Sound set", "Add sound set", true});
	{
		FieldSchema name = schema_of("name", FieldType::Text, "Name",
				"What items, ammo, scripts, sound profiles and the game itself play this set by; the game "
				"searches its banks in order for the first set of the name, without case [orig: "
				"SoundBank_FindTriggerByName @ 0x75be90].");
		name.width = kSetNameBytes;
		name.defines = ReferenceKind::Sound;
		set.field(RF{name,
		             {[](const RecordHandle &r, Value &out) { return out = set_of(r).name, true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              return set_name(r.as<BankSet>().name, kSetNameBytes, "A set's name", v, e);
		              }}});
		set.field(RF{ranged(schema_of("pitch", FieldType::Real, "Pitch",
		                              "Multiplies every member's pitch: 1 plays a wave as recorded [orig: "
		                              "SoundBank_SelectTriggerEntryFromBank @ 0x75c0be, (member * set) >> 16]."),
		                    0, 32767, 0.01),
		             {[](const RecordHandle &r, Value &out) { return out = ratio_value(set_of(r).pitch), true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              return set_ratio(r.as<BankSet>().pitch, v, "A pitch", e);
		              }}});
		set.field(RF{ranged(schema_of("pitch_jitter", FieldType::Real, "Pitch variation",
		                              "Up to this much is added to the set's pitch at random on every play "
		                              "[orig: SoundBank_SelectTriggerEntryFromBank @ 0x75c09e, (range * rand8) >> 8]."),
		                    0, 32767, 0.01),
		             {[](const RecordHandle &r, Value &out) { return out = ratio_value(set_of(r).pitch_jitter), true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              return set_ratio(r.as<BankSet>().pitch_jitter, v, "A pitch variation", e);
		              }}});
		FieldSchema range = ranged(schema_of("range", FieldType::Integer, "Heard within",
		                                     "A play at a place in the world farther than this from the listener does "
		                                     "not start at all [orig: Sound_Play3DPositional @ 0x527cd1..0x527da1 reads "
		                                     "the set's +72]; every shipped set says 10000."),
		                           0, double(INT32_MAX), 1);
		range.unit = "units";
		set.field(RF{range,
		             {[](const RecordHandle &r, Value &out) { return out = int64_t(set_of(r).range), true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              return set_whole(r.as<BankSet>().range, v, INT32_MAX, "A range", e);
		              }}});
		FieldSchema flags = schema_of("flags", FieldType::Integer, "Flags",
				"View gated: a layer plays only while its view bit matches the listener's [orig: "
				"SoundBank_PlayTriggerEntries @ 0x75cd54].");
		flags.flags = true;
		flags.choices = {{"view_gated", 1, "View gated"}};
		set.field(RF{flags,
		             {[](const RecordHandle &r, Value &out) { return out = int64_t(set_of(r).flags), true; },
		              [](const RecordHandle &r, const Value &v, std::string &e) {
			              return set_whole(r.as<BankSet>().flags, v, UINT32_MAX >> 1, "The flags", e);
		              }}});
		TableList layers;
		layers.spec = Document::CollectionSpec{kLayer, "Layers", "", false, Applicability::Reads, 8};
		layers.ops = vector_list<BankSet, BankLayer>(kLayer, [](BankSet &s) -> std::vector<BankLayer> & { return s.layers; });
		set.list(std::move(layers));
	}
	// --- a layer --------------------------------------------------------------------------------
	TableKind layer(RecordKindRow{kLayer, "layer", "Layer", "", false});
	{
		FieldSchema falloff = ranged(schema_of("falloff", FieldType::Integer, "Falloff radius",
		                                       "The volume falls to nothing at this distance: vol * (1 - d/r)^2 [orig: "
		                                       "SoundBank_CalcDistanceVolPan @ 0x75ca20]."),
		                             0, 65535, 1);
		falloff.unit = "units";
		layer.field(RF{falloff,
		               {[](const RecordHandle &r, Value &out) { return out = int64_t(layer_of(r).falloff), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                uint32_t value = 0;
			                if (!set_whole(value, v, 65535, "A radius", e)) return false;
			                r.as<BankLayer>().falloff = uint16_t(value);
			                return true;
		                }}});
		FieldSchema near = ranged(schema_of("min_distance", FieldType::Integer, "Fade-in radius",
		                                    "Inside this distance the sound fades out as the listener comes closer, "
		                                    "(d/r)^2; 0 for none [orig: SoundBank_PlayTriggerEntries @ "
		                                    "0x75cf1a..0x75cf55]."),
		                          0, 65535, 1);
		near.unit = "units";
		layer.field(RF{near,
		               {[](const RecordHandle &r, Value &out) { return out = int64_t(layer_of(r).min_distance), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                uint32_t value = 0;
			                if (!set_whole(value, v, 65535, "A radius", e)) return false;
			                r.as<BankLayer>().min_distance = uint16_t(value);
			                return true;
		                }}});
		FieldSchema flags = schema_of("flags", FieldType::Integer, "Flags",
				"How the layer picks its member and when it is heard: in order (0x10) first, then a random "
				"start (0x80), else at random; a play admits it only in a view it is heard in [orig: "
				"SoundBank_PlayTriggerEntries @ 0x75cd54, @ 0x75cd5c..0x75cdfc].");
		flags.flags = true;
		flags.choices = layer_flag_choices();
		layer.field(RF{flags,
		               {[](const RecordHandle &r, Value &out) { return out = int64_t(layer_of(r).flags), true; },
		                [](const RecordHandle &r, const Value &v, std::string &e) {
			                return set_whole(r.as<BankLayer>().flags, v, 0xFFFF, "The flags", e);
		                }}});
		FieldSchema picks = schema_of("picks", FieldType::Text, "Picks a member",
				"How the game picks the member it plays, by the flags [orig: SoundBank_PlayTriggerEntries @ "
				"0x75cd5c..0x75cdfc].");
		picks.read_only = true;
		layer.field(RF{picks, {[](const RecordHandle &r, Value &out) {
			              return out = std::string(layer_selection_words(layer_of(r).flags)), true;
		              }}});
		TableList members;
		members.spec = Document::CollectionSpec{kMember, "Members", "wave", false, Applicability::Reads, 8};
		members.ops = vector_list<BankLayer, BankMember>(kMember,
				[](BankLayer &l) -> std::vector<BankMember> & { return l.members; });
		layer.list(std::move(members));
	}
	// --- a member -------------------------------------------------------------------------------
	TableKind member(RecordKindRow{kMember, "member", "Member", "", false});
	{
		FieldSchema wave_name = schema_of("wave", FieldType::Text, "Wave",
				"The wave of this bank the member plays, by its name.");
		wave_name.width = kWaveNameBytes;
		wave_name.reference = ReferenceKind::BankWave;
		member.field(RF{wave_name,
		                {[](const RecordHandle &r, Value &out) { return out = member_of(r).wave, true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 return set_name(r.as<BankMember>().wave, kWaveNameBytes, "A wave's name", v, e);
		                 }}});
		member.field(RF{ranged(schema_of("pitch", FieldType::Real, "Pitch",
		                                 "1 plays the wave as recorded; the set's pitch multiplies it [orig: "
		                                 "SoundBank_SelectTriggerEntryFromBank @ 0x75c109]."),
		                       0, 32767, 0.01),
		                {[](const RecordHandle &r, Value &out) { return out = ratio_value(member_of(r).pitch), true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 return set_ratio(r.as<BankMember>().pitch, v, "A pitch", e);
		                 }}});
		member.field(RF{ranged(schema_of("pitch_jitter", FieldType::Real, "Pitch variation",
		                                 "Up to this much is added to the member's pitch at random on every play "
		                                 "[orig: SoundBank_PlayTriggerEntries @ 0x75cedc]."),
		                       0, 32767, 0.01),
		                {[](const RecordHandle &r, Value &out) { return out = ratio_value(member_of(r).pitch_jitter), true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 return set_ratio(r.as<BankMember>().pitch_jitter, v, "A pitch variation", e);
		                 }}});
		member.field(RF{ranged(schema_of("volume", FieldType::Integer, "Volume",
		                                 "0 to 255 at the source, before the distance takes its share [orig: "
		                                 "SoundBank_PlayTriggerEntries @ 0x75cf25]."),
		                       0, 255, 1),
		                {[](const RecordHandle &r, Value &out) { return out = int64_t(member_of(r).volume), true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 return set_whole(r.as<BankMember>().volume, v, 255, "A volume", e);
		                 }}});
		member.field(RF{ranged(schema_of("ceiling", FieldType::Integer, "Volume ceiling",
		                                 "The most the distance-scaled volume reaches, 0 to 255; also how far it "
		                                 "pans [orig: SoundBank_CalcDistanceVolPan @ 0x75ca65]."),
		                       0, 255, 1),
		                {[](const RecordHandle &r, Value &out) { return out = int64_t(member_of(r).ceiling), true; },
		                 [](const RecordHandle &r, const Value &v, std::string &e) {
			                 return set_whole(r.as<BankMember>().ceiling, v, 255, "A volume ceiling", e);
		                 }}});
	}
	return RecordTable({std::move(wave), std::move(set), std::move(layer), std::move(member)});
}

const SoundBankRow &bank_row(const Node &node) { return static_cast<const SoundBankRow &>(node); }

// A name no row of the kind among `rows` has (the game finds the first of two of one name).
std::string free_name(const std::vector<std::shared_ptr<const Node>> &rows, NodeKind kind, const std::string &stem,
                      size_t bytes, const Node *self = nullptr) {
	const auto taken = [&](const std::string &name) {
		for (const auto &other : rows)
			if (other && other.get() != self && other->kind == kind && strutil::iequals(other->name(), name))
				return true;
		return false;
	};
	if (!taken(stem)) return stem;
	for (int n = 2; n < 100000; ++n) {
		const std::string suffix = "_" + std::to_string(n);
		const std::string name = stem.substr(0, std::min(stem.size(), bytes - 1 - suffix.size())) + suffix;
		if (!taken(name)) return name;
	}
	return stem;
}

} // namespace

const RecordTable &sound_bank_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_sound_bank_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::SoundBank; }

std::string sound_bank_scope(const std::string &path) { return strutil::to_upper(basename_of(path)); }

const char *layer_selection_words(uint32_t flags) {
	if (flags & lwf::kFlagSequential) return "in order";
	if (flags & lwf::kFlagRandomSequential) return "a random start, then in order";
	return "at random";
}

// --- the row -------------------------------------------------------------------------------------

SoundBankRow::SoundBankRow(NodeKind k) { kind = k; }

std::string SoundBankRow::name() const { return kind == kWave ? wave.name : set.name; }

RecordHandle SoundBankRow::record() const {
	return kind == kWave ? RecordHandle{kind, const_cast<BankWave *>(&wave)} : RecordHandle{kind, const_cast<BankSet *>(&set)};
}

size_t SoundBankRow::footprint() const {
	size_t bytes = sizeof(SoundBankRow) + footprint_of(wave.name) + footprint_of(wave.file) + footprint_of(set.name) +
	               footprint_of(set.layers) + ids_footprint();
	for (const BankLayer &layer : set.layers) {
		bytes += footprint_of(layer.members);
		for (const BankMember &member : layer.members) bytes += footprint_of(member.wave);
	}
	return bytes;
}

// --- the document ----------------------------------------------------------------------------------

std::vector<const SoundBankRow *> SoundBankDocument::rows_of(SoundBankKind kind) const {
	std::vector<const SoundBankRow *> out;
	for (const auto &node : rows())
		if (node && node->kind == node_kind(kind)) out.push_back(&bank_row(*node));
	return out;
}

const SoundBankRow *SoundBankDocument::find_set(const std::string &name) const {
	for (const SoundBankRow *row : rows_of(SoundBankKind::Set))
		if (strutil::iequals(row->set.name, name)) return row;
	return nullptr;
}

std::string SoundBankDocument::record_title(const NodeAddress &address) const {
	const Node *node = row(address.row);
	if (!node) return std::string();
	if (!address.child) {
		const SoundBankRow &r = bank_row(*node);
		if (r.kind == kWave) return r.wave.file.empty() ? r.wave.name : r.wave.name + " (" + r.wave.file + ")";
		return r.set.name;
	}
	if (address.kind == kLayer) {
		const RecordHandle handle = record_in(*node, address);
		if (!handle) return record_name(address);
		const BankLayer &layer = layer_of(handle);
		const size_t count = layer.members.size();
		return record_name(address) + ": " + layer_selection_words(layer.flags) + ", " + std::to_string(count) +
		       (count == 1 ? " member" : " members");
	}
	return record_name(address);
}

bool SoundBankDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                              std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues,
                              Diagnostic &error) {
	if (!is_sound_bank_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not a sound bank.", path());
		return false;
	}
	lwf::File file;
	std::string message;
	if (!lwf::parse_lwf_buffer(bytes.data(), bytes.size(), file, message)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     "The sound bank could not be read: " + message + ".", path());
		return false;
	}
	if (file.header.magic != lwf::kMagic)
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The bank's magic is not LWF1, so the game gives its waves no file [orig: "
		                  "SoundBank_LoadTriggerSets @ 0x75c671]: a save writes LWF1 and each wave's file."});
	if (!file.triggers.empty())
		issues.push_back({false, 0, std::string(), std::string(),
		                  "The bank holds " + std::to_string(file.triggers.size()) +
		                          " trigger records, which the game reads and never uses [orig: SoundBank_OpenFile @ "
		                          "0x75cb7a]: a save leaves them out."});
	// The first wave of each name: a member names its wave by its name, which the save finds the
	// first of, as the bank's own lookups do.
	std::unordered_map<std::string, size_t> first_of;
	for (size_t i = 0; i < file.singles.size(); ++i) {
		const lwf::Single &single = file.singles[i];
		first_of.emplace(strutil::to_lower(single.name), i);
		auto row = std::make_shared<SoundBankRow>(kWave);
		row->wave.name = single.name;
		row->wave.file = single.path;
		row->wave.volume = uint32_t(single.value_hi >> 8);
		shape(*row);
		rows.push_back(std::move(row));
	}
	size_t repointed = 0;
	for (const lwf::Multi &multi : file.multis) {
		auto row = std::make_shared<SoundBankRow>(kSet);
		BankSet &set = row->set;
		set.name = multi.name;
		set.pitch = multi.pitch_base;
		set.pitch_jitter = multi.pitch_random_range;
		set.range = multi.target_id;
		set.flags = multi.set_flags;
		for (const uint32_t p : multi.playlist_indices) {
			const lwf::Playlist &playlist = file.playlists[p];
			BankLayer layer;
			layer.falloff = playlist.falloff_radius;
			layer.min_distance = playlist.min_distance;
			layer.flags = playlist.flags;
			for (const uint32_t s : playlist.sndparm_indices) {
				const lwf::Sndparm &sndparm = file.sndparms[s];
				BankMember member;
				const std::string &wave = file.singles[sndparm.single_index].name;
				if (first_of[strutil::to_lower(wave)] != sndparm.single_index) ++repointed;
				member.wave = wave;
				member.pitch = sndparm.pitch_scaled;
				member.pitch_jitter = sndparm.random_pitch_scaled;
				member.volume = sndparm.volume;
				member.ceiling = sndparm.clamp_volume;
				layer.members.push_back(std::move(member));
			}
			set.layers.push_back(std::move(layer));
		}
		shape(*row);
		rows.push_back(std::move(row));
	}
	if (repointed)
		issues.push_back({false, 0, std::string(), std::string(),
		                  std::to_string(repointed) + " member" + (repointed == 1 ? " plays a wave" : "s play waves") +
		                          " named like an earlier wave of the bank: the bank finds a wave by its first of the "
		                          "name, so a save points " + (repointed == 1 ? "it" : "them") + " at the first."});
	return true;
}

bool SoundBankDocument::bank(lwf::File &file, std::vector<SourceIssue> *issues) const {
	file = lwf::File();
	file.header.magic = lwf::kMagic;
	std::unordered_map<std::string, uint32_t> first_of;
	for (const SoundBankRow *row : rows_of(SoundBankKind::Wave)) {
		lwf::Single single;
		single.name = row->wave.name;
		single.path = row->wave.file;
		single.value_hi = uint16_t((row->wave.volume & 0xFF) << 8);
		first_of.emplace(strutil::to_lower(single.name), uint32_t(file.singles.size()));
		file.singles.push_back(std::move(single));
	}
	bool ok = true;
	for (const SoundBankRow *row : rows_of(SoundBankKind::Set)) {
		lwf::Multi multi;
		multi.name = row->set.name;
		multi.pitch_base = row->set.pitch;
		multi.pitch_random_range = row->set.pitch_jitter;
		multi.target_id = row->set.range;
		multi.set_flags = row->set.flags;
		for (size_t l = 0; l < row->set.layers.size(); ++l) {
			const BankLayer &layer = row->set.layers[l];
			lwf::Playlist playlist;
			playlist.falloff_radius = layer.falloff;
			playlist.min_distance = layer.min_distance;
			playlist.flags = layer.flags;
			for (size_t m = 0; m < layer.members.size(); ++m) {
				const BankMember &member = layer.members[m];
				const auto wave = first_of.find(strutil::to_lower(member.wave));
				if (wave == first_of.end()) {
					ok = false;
					if (issues)
						issues->push_back({true, 0, row->set.name, "wave",
						                   row->set.name + ", layer " + std::to_string(l + 1) + ", member " +
						                           std::to_string(m + 1) + " plays the wave '" + member.wave +
						                           "', which this bank has none of: the file names a member's wave by "
						                           "its place in the bank. Add the wave, or play another."});
					continue;
				}
				lwf::Sndparm sndparm;
				sndparm.single_index = wave->second;
				sndparm.pitch_scaled = member.pitch;
				sndparm.random_pitch_scaled = member.pitch_jitter;
				sndparm.volume = member.volume;
				sndparm.clamp_volume = member.ceiling;
				playlist.sndparm_indices.push_back(uint32_t(file.sndparms.size()));
				file.sndparms.push_back(sndparm);
			}
			multi.playlist_indices.push_back(uint32_t(file.playlists.size()));
			file.playlists.push_back(std::move(playlist));
		}
		file.multis.push_back(std::move(multi));
	}
	return ok;
}

SerializeResult SoundBankDocument::serialize() const {
	SerializeResult result;
	lwf::File file;
	if (!bank(file, &result.issues)) return result;
	std::vector<uint8_t> bytes;
	std::string error;
	if (!lwf::encode_lwf(file, bytes, error)) {
		result.issues.push_back({true, 0, std::string(), std::string(), "The sound bank could not be written: " + error + "."});
		return result;
	}
	result.text.assign(bytes.begin(), bytes.end());
	return result;
}

std::string SoundBankDocument::save_words() const {
	return "A save writes the bank from its waves and sets: each member pointing at the first wave of its name, "
	       "no trigger record, every unused word zero.";
}

std::shared_ptr<Node> SoundBankDocument::make_node(NodeKind kind, NodeId, const std::vector<std::shared_ptr<const Node>> &rows,
                                                   std::string &error) {
	if (kind != kWave && kind != kSet) {
		error = "A sound bank's rows are its waves and its sets.";
		return nullptr;
	}
	auto row = std::make_shared<SoundBankRow>(kind);
	if (kind == kWave) {
		row->wave.name = free_name(rows, kWave, "NEWWAVE", kWaveNameBytes);
	} else {
		row->set.name = free_name(rows, kSet, "NEW_SET", kSetNameBytes);
	}
	shape(*row);
	return row;
}

size_t SoundBankDocument::row_position(const Node &row, const std::vector<std::shared_ptr<const Node>> &rows,
                                       size_t position) const {
	// The waves' band, then the sets': among the other rows (a moved row passed over).
	size_t waves = 0, others = 0;
	for (const auto &other : rows) {
		if (other.get() == &row) continue;
		++others;
		if (other->kind == kWave) waves = others;
	}
	return row.kind == kWave ? std::min(position, waves) : std::max(std::min(position, others), waves);
}

void SoundBankDocument::prepare_duplicate(Node &copy, const Node &original,
                                          const std::vector<std::shared_ptr<const Node>> &rows) const {
	SoundBankRow &r = static_cast<SoundBankRow &>(copy);
	if (r.kind == kWave) r.wave.name = free_name(rows, kWave, bank_row(original).wave.name, kWaveNameBytes, &copy);
	else r.set.name = free_name(rows, kSet, bank_row(original).set.name, kSetNameBytes, &copy);
}

void SoundBankDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	// A set is found by name in every bank the game searches (an unscoped lookup) and by a menu's SOUND in
	// its own bank, which this scope names.
	if (use.defines == ReferenceKind::BankWave || use.reference == ReferenceKind::BankWave ||
	    use.defines == ReferenceKind::Sound)
		use.scope = sound_bank_scope(path());
}

void SoundBankDocument::after_add(Node &, const ListChange &, const RecordHandle &made) {
	if (made.kind != kMember) return;
	const std::vector<const SoundBankRow *> waves = rows_of(SoundBankKind::Wave);
	if (!waves.empty()) made.as<BankMember>().wave = waves.front()->wave.name;
}

// --- the findings ------------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<SoundBankFinding> kFindingEntries[] = {
	{ SoundBankFinding::InvalidInput, { "sound_bank.invalid_input", FindingFix::None, nullptr, true } },
	{ SoundBankFinding::IgnoredInput, { "sound_bank.ignored_input", FindingFix::Rewrite, kRewriteDropsIgnoredInput } },
	// The bank's lookups find the first of a name [orig: SoundBank_FindEntryByName @ 0x75bba0]: the later
	// one is played by no member the editor writes, and no dialog line finds it. No refusal is witnessed.
	{ SoundBankFinding::WaveNameRepeated, listed_code("sound_bank.wave_name_repeated") },
	{ SoundBankFinding::SetNameRepeated, listed_code("sound_bank.set_name_repeated") },
	{ SoundBankFinding::WaveNoFile, listed_code("sound_bank.wave_no_file") },
	{ SoundBankFinding::WaveFileName, listed_code("sound_bank.wave_file_name") },
	{ SoundBankFinding::Unserializable, { "sound_bank.unserializable", FindingFix::None, nullptr, true } },
	{ SoundBankFinding::LayerUnheard, listed_code("sound_bank.layer_unheard") },
	{ SoundBankFinding::SetSilent, listed_code("sound_bank.set_silent") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(SoundBankFinding::kCount),
		"every SoundBankFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
		"the sound bank's rows follow SoundBankFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::SoundBanks);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(SoundBankFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable sound_bank_finding_codes() { return { kFindingRows.data(), kFindingRows.size() }; }

// A sound set nothing defines where the game searches (DI-15): a new set of the bank, named as the reference
// names it (a set's name is its first 23 characters [orig: SoundBank_LoadTriggerSets @ 0x75c43e..0x75c461]), as
// Add set makes one: the authoring tools' stock pitch base and every shipped set's range (BankSet's defaults), no
// layer yet, so it plays nothing until one is given, as nothing plays for the name now.
bool define_sound_set(const DocumentBase &document, const ReferenceSubject &missing, PlannedFix &out) {
	const auto *bank = dynamic_cast<const SoundBankDocument *>(&document);
	if (!bank || missing.kind != ReferenceKind::Sound || missing.target.empty() || missing.target.size() >= kSetNameBytes)
		return false;
	const std::string file = basename_of(document.path());
	// A menu SOUND's set is looked up in the bank its SOUND names alone.
	if (!missing.scope.empty() && !strutil::iequals(missing.scope, file)) return false;
	Edit add;
	add.operation = EditOperation::Add;
	add.address.kind = kSet;
	add.field = "name";
	add.value = missing.target;
	out = PlannedFix();
	out.edits.push_back(std::move(add));
	out.label = "Add " + missing.target + " to " + file;
	out.detail = "Adds the sound set " + missing.target + " to " + file + ", as Add sound set makes one (the stock pitch, a range "
	             "of 10000), and selects it to give it a layer and the waves it plays: until then it plays nothing, as "
	             "nothing plays for the name now.";
	return true;
}

std::vector<Diagnostic> validate_sound_bank_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *bank = dynamic_cast<const SoundBankDocument *>(&document);
	if (!bank) return findings;
	source_issue_findings(*bank, finding_code(SoundBankFinding::InvalidInput),
	                      finding_code(SoundBankFinding::IgnoredInput), findings);
	if (document.blocked()) return findings;
	const auto add = [&](const NodeAddress &address, const std::string &record, DiagnosticSeverity severity,
	                     SoundBankFinding code, const char *field, const std::string &message) {
		Diagnostic d = make_finding(code, severity, message, document.path(), field);
		d.row_id = address.row;
		d.child_id = address.child;
		d.record_kind = address.kind;
		d.record = record;
		findings.push_back(std::move(d));
	};
	std::unordered_map<std::string, std::string> waves;
	for (const SoundBankRow *row : bank->rows_of(SoundBankKind::Wave)) {
		const NodeAddress at{row->id, kWave, 0};
		const BankWave &wave = row->wave;
		if (!waves.emplace(strutil::to_lower(wave.name), wave.name).second)
			add(at, wave.name, DiagnosticSeverity::Warning, SoundBankFinding::WaveNameRepeated, "name",
			    "An earlier wave of the bank is named '" + wave.name +
			            "': the bank finds the first of a name, so no member and no dialog line plays this one.");
		if (wave.file.empty())
			add(at, wave.name, DiagnosticSeverity::Warning, SoundBankFinding::WaveNoFile, "file",
			    "The wave names no file: a member that plays it plays nothing.");
		else if (const std::string flat = io::utf8_file_name(wave.file); !pff::logical_name_fits_archive(flat))
			// The name the archives are searched by, cut at either separator on every host (a shipped bank's
			// files are "SFX\\MENU\\SELECTA1.wav"; the graph's wave_files reads the same name).
			add(at, wave.name, DiagnosticSeverity::Warning, SoundBankFinding::WaveFileName, "file",
			    "The file name " + flat + " does not fit the game's archives (16 characters at "
			    "most), so the game never finds it there.");
	}
	std::unordered_map<std::string, std::string> sets;
	for (const SoundBankRow *row : bank->rows_of(SoundBankKind::Set)) {
		const NodeAddress at{row->id, kSet, 0};
		const BankSet &set = row->set;
		if (!sets.emplace(strutil::to_lower(set.name), set.name).second)
			add(at, set.name, DiagnosticSeverity::Warning, SoundBankFinding::SetNameRepeated, "name",
			    "An earlier set of the bank is named '" + set.name + "': the game plays the first set of a name.");
		const Node &node = *row;
		static const std::vector<RecordIds> kNone;
		const std::vector<RecordIds> &layer_ids = row->ids.lists.empty() ? kNone : row->ids.lists[0];
		size_t members = 0;
		for (size_t l = 0; l < set.layers.size(); ++l) {
			const BankLayer &layer = set.layers[l];
			members += layer.members.size();
			const NodeAddress layer_at{node.id, kLayer, l < layer_ids.size() ? layer_ids[l].id : 0};
			if (!(layer.flags & (lwf::kFlagInternal | lwf::kFlagExternal)))
				add(layer_at, set.name, DiagnosticSeverity::Warning, SoundBankFinding::LayerUnheard, "flags",
				    set.name + "'s layer " + std::to_string(l + 1) +
				            " is heard in neither view, so no play admits it [orig: SoundBank_PlayTriggerEntries @ "
				            "0x75cd54]: give it the first-person view, the outside view or both.");
			for (size_t m = 0; m < layer.members.size(); ++m) {
				const BankMember &member = layer.members[m];
				if (waves.count(strutil::to_lower(member.wave))) continue;
				const std::vector<RecordIds> &member_ids =
						l < layer_ids.size() && !layer_ids[l].lists.empty() ? layer_ids[l].lists[0] : kNone;
				add(NodeAddress{node.id, kMember, m < member_ids.size() ? member_ids[m].id : 0}, set.name,
				    DiagnosticSeverity::Error, SoundBankFinding::Unserializable, "wave",
				    member.wave.empty()
				            ? set.name + ", layer " + std::to_string(l + 1) + ": a member plays no wave, which the file "
				                                                              "cannot hold: pick one of the bank's waves."
				            : set.name + ", layer " + std::to_string(l + 1) + ": a member plays '" + member.wave +
				                      "', which no wave of the bank is named: the bank cannot be saved until it is.");
			}
		}
		if (members == 0)
			add(at, set.name, DiagnosticSeverity::Info, SoundBankFinding::SetSilent, "",
			    set.name + " has no member to play: playing it plays nothing.");
	}
	return findings;
}

} // namespace opennova::editor
