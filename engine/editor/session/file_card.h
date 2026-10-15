#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <base/io/json.h>
#include <editor/assets/asset_kind.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/value.h>

namespace opennova::editor {

struct AssetScan;
struct GraphEdge;
struct SessionView;

// A project file as a modder asks after it (the UX round's project lane and its plain-words lane; the audit's
// 4.6: a double-click on a texture, a sound bank or charattr.def did nothing and said nothing): one model, which
// Files' card (a window of its own, an about_file's) and the Document window's page of a file the editor has no
// editor for (an open_document's: ui/file_page_view) both draw, and the editor MCP's file_card query answers.
// What it is (its kind's words, assets/asset_kind_words: what a file of the kind holds and what in the game reads
// it and when, cited), where a build puts it, what the editor does with it, where it came from, what it defines
// with who names each (DI-17), what it names (each reference with whether the project resolves it and the file it
// does) and who names it, each line a Go to; its Problems rows; a wave's sound as the game decodes it
// (lwf::wav_decode_pcm16: its channels, rate and length), which the Shell plays (play_sound). A Go to whose
// target the editor does not edit lands on the page (DI-17): the line of the record it names is marked.
struct FileCard {
	bool found = false;
	std::string path; // project-relative
	std::string name; // logical
	AssetKind kind = AssetKind::Unknown;
	std::string kind_label;
	// What a file of its kind holds, what in the game reads it and when, and the witness of that
	// (assets/asset_kind_words).
	std::string what, read_by, cite;
	uint64_t size = 0;
	std::string build;  // where a build puts it, in words
	std::string editor; // what the editor does with it ("The editor opens it as a document.")
	std::string imported_from;
	bool opens = false; // the editor opens a document of it
	// A wave: what the game's decode makes of it, read once for the file's size and last write (a card made
	// again over the same file takes it as it is), and not for a file past kWaveCardBytes.
	struct Sound {
		bool decoded = false;
		std::string error; // why it does not decode
		uint32_t rate = 0;
		uint16_t channels = 0;
		double seconds = 0.0;
		// What the game's loader makes of it (the sound lane, formats/lwf/wav_source.h wave_retail_check): whether
		// it plays, and why not; its format in words ("16-bit PCM, mono, 22050 Hz"), its loudest sample and its
		// RMS (each 0..1 of full scale) and its picture, each bin's loudest sample (kWaveCardBins of them).
		bool plays = false;
		std::string refusal;
		std::string format;
		float peak = 0.0f;
		float rms = 0.0f;
		std::vector<float> envelope;
		uint64_t size = 0;     // the file's, as read
		int64_t modified = 0;  // its last-write ticks, as read (0: unknown, read again)
	};
	bool wave = false;
	Sound sound;
	// Where a Go to landed on it (DI-17): the record (a ReferenceTarget's locator: a record of the file by its
	// locator, else its path as the graph names it) and its field; both "" none.
	std::string at_locator, at_field;
	// A line of it somewhere to go: its words, whether it is the record the Go to that showed it marked, and
	// where a click goes (the file opened at the record, or its page).
	struct Line {
		std::string text;
		bool at = false;
		ReferenceTarget target;
	};
	// What it names: the line's words ("polytrn_colormap: texture isle_c.tga", where in the file it is named: its
	// record, else its field), the field's words, the record's, the value, whether the project has it and the
	// file it resolves to ("" for none, or a name no file is: a symbol), that file a wave. A name nothing
	// resolves goes where it belongs (missing_target: a symbol's file); a missing file's goes nowhere (Problems
	// shows its finding, with its fixes).
	struct Named : Line {
		std::string field;
		std::string record;
		std::string value;
		ReferenceStatus status = ReferenceStatus::NotAReference;
		std::string file;
		bool wave = false;
		bool missing() const { return status == ReferenceStatus::Missing; }
	};
	// Who names it, or what it defines: the line's words ("ITEMS.DEF: Drivable Dune Buggy - Shadow texture"),
	// the naming file, its record and the field's words.
	struct User : Line {
		std::string file;
		std::string record;
		std::string field;
	};
	// A name it defines (graph/reference_queries' file_definitions): its words, whether a lookup of the game
	// finds it, and the uses that reach it.
	struct Definition {
		std::string text;   // "Particle effect Effect_AmHitDirt"
		bool read = true;   // a lookup of the game finds it (false: inert)
		std::string unread; // why none does, in a few words
		bool at = false;    // the definition a Go to landed on
		std::vector<User> used_by;
	};
	std::vector<Definition> defines; // what it defines that others name
	std::vector<Named> names;        // what it names, each a reference
	std::vector<User> used_by;       // the records naming it, then those naming what it defines
	size_t errors = 0, warnings = 0; // its Problems rows
	// The project's references are being read (a validation runs, or none has read them yet): what it
	// names and who names it are the graph's as far as it has read, and may grow (the demo round's bug 9:
	// "Named by (0)" while an import's files were read).
	bool reading = false;
};

// Who names a project file, in one look (the deep-integration plan's DI-05): the card's Used by, made
// without a card (no wave read, nothing it names), for the Inspector's view of a file with nothing selected,
// the document toolbars' Used by chip and the wire's used_by query. Its usages (graph/reference_queries'
// file_uses: the records naming it, then those naming what it defines) grouped by the file they are in, in
// the graph's order, each a place in words (edge_place_words: the record in its type's words, the field by
// its label) with where Go to takes it (usage_target); and one hop further where the naming record is a
// definition itself (oncrate1.3di named by the item Wooden supply crate in items.def, which onjo_m1.bms
// places six times), those uses grouped by their file too.
struct FileUsers {
	bool found = false;
	std::string path; // project-relative
	std::string name; // logical
	// A place that names it (or names what names it).
	struct Line {
		std::string file;  // the naming file, project-relative
		std::string words; // the record and its field, in words ("Wooden supply crate - Graphic")
		std::string field; // the field's id
		ReferenceTarget target;
	};
	struct Group {
		std::string file;
		std::string name; // the file's logical name
		std::vector<Line> lines;
	};
	struct Use {
		Line line;
		std::vector<Group> further; // one hop further, by file
		size_t further_count = 0;
	};
	struct UseGroup {
		std::string file;
		std::string name;
		std::vector<Use> uses;
	};
	std::vector<UseGroup> groups;
	size_t count = 0;         // its uses (the first hop's lines)
	size_t further_count = 0; // the lines one hop further
	// The project's references are being read (FileCard::reading): the lists are the graph's as far as it has
	// read, and may grow.
	bool reading = false;
};

// Who names `path` (a project-relative path or a logical name); found false for a file the project lacks.
FileUsers file_users(const SessionView &view, const std::string &path);
// How many uses it has (FileUsers::count, AssetGraph::usages_of), nothing worded: the toolbars' chip.
size_t file_use_count(const SessionView &view, const std::string &path);
// Its wire form, the used_by query's: {found, path, name, count, further_count, reading?, files [{file, name,
// uses [{words, field, file, locator?, editable, further [{file, name, uses [{words, field, file, locator?,
// editable}]}]}]}]}; a use's file and locator are where Go to opens it (open_document's path and locator: the
// file's page with the record marked where `editable` is false, DI-17).
io::JsonValue file_users_json(const FileUsers &users);

// The most of a wave a card reads to say what it is (the game's own are a few hundred KB): a larger file is
// said to be too large, nothing read.
inline constexpr uint64_t kWaveCardBytes = uint64_t(32) << 20;
// The bins of a wave's picture on its card.
inline constexpr size_t kWaveCardBins = 64;

// The card of the project file `path` (a project-relative path or a logical name); found false for none.
// `known`, a card's sound read before: taken as it is while the file's size and last write are those it was
// read at, else read again. `locator` and `field`, the record a Go to names on it (a ReferenceTarget's: a
// record of the file by its locator, else its path as the graph names it; both "" none), whose lines are
// marked.
FileCard file_card(const SessionView &view, const std::string &path, const FileCard::Sound *known = nullptr,
                   const std::string &locator = std::string(), const std::string &field = std::string());
// The same, with the record the page's Go to marked where `path` is the page the Document window shows.
FileCard shown_file_card(const SessionView &view, const std::string &path, const FileCard::Sound *known = nullptr);
// Its wire form, the file_card query's: {found, and with one path, name, kind (its token), kind_label, size,
// what, read_by, cite, build, editor, imported_from, opens, errors, warnings, wave? (true for a wave), sound?
// {decoded, error?, rate, channels, seconds, plays, refusal?, format, peak, rms, envelope}, at_locator?,
// at_field?, defines [{text, read, unread?, at?, used_by}], names [{text, value, status, wave?, missing?, at?,
// file?, locator?, field?}], used_by [{text, file?, locator?, field?}], reading? (true while the project's
// references are being read)}; a line's file, locator and field are where a click goes (open_document's).
io::JsonValue file_card_json(const FileCard &card);

// A graph edge's field by the name the Inspector shows it under (its type's schema, which no file's
// content changes), its id where the file's kind has no document type or the schema no such field.
std::string edge_field_words(const AssetScan &scan, const GraphEdge &edge);

// A reference's state in words: "found in the project", "missing", "not checked".
const char *reference_status_words(ReferenceStatus status);

} // namespace opennova::editor
