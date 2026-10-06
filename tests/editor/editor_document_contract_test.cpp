// S12 Z (ADR 0046 S12, "Adding a document type"): the contract every registered document type
// keeps, checked over a file of its kinds. The types are the registry's own (every asset kind
// document_type_for answers, each type once), so a new type fails here until it has a file below.
// Over each file: the document loads unblocked; parse, serialize, parse again serializes the same
// bytes; every field of every record reads, carries a label (a def member only its line does not
// write as a number of its own may go by its id), a range that is one (a number's, low to high),
// the same id and type as the record's field_on gives it, and field_on never makes a read-only
// field writable; a writable field set to the value it reads leaves the serialized bytes as they
// were; every field a record defines a name by yields a symbol whose locator names that record,
// and find_definition resolves its name in a scope only to a definition of its kind and name
// there (nothing in a scope with none), the same over the document alone and over a graph whose
// slot is current for it (S13 D3); every reference and definition kind has its
// reference_kinds row. S13 D2 adds what needs no multi-row edit: every record's locator finds it
// again, and two loads of the file give every record the same identity; an optional field the
// game reads on its record left out and written again (Clear, Write), a Clear of one left out
// and a Write of one written no step, the undo giving the bytes back (a type refuses one only as
// always written; how many each type is asked and does is pinned); a real change of a value,
// the bytes before, after, after its undo and after its redo; the record and the field changed
// since the save (no other row's record), and the edits that give the field back (revert_edits)
// giving it its saved value, and the record and the bytes as the file held them, which every
// file reaches (a def member whose Set changes another of its record's, def_sync_derived, an open
// item, is checked but for its bytes, and the clause goes on to another field); two coalesced
// Sets of a field one undo step; a record copied and pasted where the type copies records, the
// undo giving the bytes back; and a snapshot of the changed document, which serializes its
// bytes, shares its identity, revision and records, answers record_change and field_changed as
// it does, refuses an edit, a save and a load (document.snapshot), and after its document's
// undo still finds every record where it was and answers what changed in it as it did. S13 D5
// adds the type's record kinds (kinds()): each named back by its token, no two sharing a kind or
// a token, a kind the outline adds a row of being a row of the file; every row of the file of a
// kind that is a row, and every record a collection holds of a kind the table has. S13 V3 adds that
// a kind is one record kind across every asset kind the type opens (the same token in each: the
// type's schema without a document, DocumentType::fields, answers by the kind alone), whose fields
// are the type's fields(kind), the very table its documents answer. S13 D4 adds the
// type's validate_file: the file's own findings from its document alone, each on the file and on
// a record the document holds, the same findings from a second load of the file, and a finding
// over each type's files (a flawed file of its own where its fixture has no flaw). S13 D6: the
// type's make gives a DocumentBase whose record document (as_records) is itself; a change of a
// kind the type did not make (an Apply of another's payload) is refused (document.payload),
// nothing committed; a snapshot shares its document's load generation; and what the type declares
// it adds is added (make_node and edit_collection refuse by default): a row of every kind the
// outline adds at the end, and a record into every kind of collection the file's records hold that
// is not fixed (each owner kind and record kind once, a full one skipped; refused only by a rule of
// the type's own), what serializes reading back with the record kept (a record the writer takes
// only once filled in counted as waiting), a Remove giving the owner the records it held, and each
// undo the bytes. S13 D7 adds the batch over several rows: every row's footprint at least its own
// object (its row type's, which the contract names), and a row whose text field takes a longer
// text larger by at least the text it gained (each writable text field of each record kind once);
// a real change of two rows in one batch one step (the history holding its rows' bytes,
// what changed since the state before it those two rows), undone to the bytes and redone to the
// bytes it made, and the same two Sets as a gesture's two batches one step; and a batch mixing a
// record's Set with the rows' own edits (a row of each kind the outline adds, the last row
// duplicated, the last row moved to the top) one step, undone and redone byte for byte with
// everything it made listed, or refused by a rule of the type's own with nothing committed. S13
// V9: where the type has a project check, over its file in a project of its own, a second update
// with nothing changed says nothing moved and keeps its findings, and clear() then an update makes
// the same findings again. S13 D8: where the type's schema names a Record reference (an index into
// a collection of its own file), every reference names a record of the file's record set, and a
// record added where the first record named stands leaves each naming the record it named (the
// type renumbering them in the same step), its removal and each undo giving the bytes back. S13 D9:
// a text type's make gives a DocumentBase whose text document (as_text) is itself and that holds no
// records; over its file: it loads unblocked and serializes the bytes it was read from, parse,
// serialize, parse again the same bytes and text; its validate_file's findings on its file alone, a
// second load validating to the same; a change of a kind the type did not make, or an edit naming a
// record, refused with nothing committed; a span replaced a real change (the bytes another, its undo
// giving them back, its redo the change, what changed since the load that span), two coalesced
// replacements one step, a gesture's two batches one step whose change set holds both spans; and a
// snapshot sharing its identity, load and revision, serializing its bytes and refusing an edit, a
// save and a load. Every text type but the text one (whose files the game reads through readers the
// editor does not model) makes a finding over its files. S18: an image type's make gives a DocumentBase
// that holds an image (the texture's, over a minted TGA, PCX and DDS): it loads unblocked, serializes the
// bytes it was read from, validates alike twice, refuses another's change and an edit naming a record,
// says nothing changed since its load, and snapshots as a text's does; the texture type makes no
// finding yet (what the game makes of a texture is its role's), its table empty.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <typeinfo>
#include <utility>
#include <variant>
#include <vector>

#include <base/io/strutil.h>
#include <editor/assets/asset_kind.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/animation_document.h>
#include <editor/documents/animation_map_document.h>
#include <editor/documents/def_catalog_document.h>
#include <editor/documents/document_types.h>
#include <editor/documents/environment_document.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mns_document.h>
#include <editor/documents/mnu_document.h>
#include <editor/documents/model_document.h>
#include <editor/documents/project_check.h>
#include <editor/documents/strings_document.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/reference_kinds.h>
#include <editor/graph/reference_queries.h>
#include <editor/model/document.h>
#include <editor/model/text_document.h>
#include <editor/project/project_document.h>
#include <formats/bad/bad.h>
#include <formats/bad/bad_write.h>
#include <formats/cbin/binary_config.h>
#include <formats/dds/dds.h>
#include <formats/def/def_schema.h>
#include <formats/mission/bms.h>
#include <formats/mus/mus.h>
#include <formats/pcx/pcx_io.h>
#include <formats/rtxt/rtxt.h>
#include <formats/scr/scr.h>
#include <formats/tga/tga.h>

#include "common/file_io.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;

namespace {

int g_failures = 0;
// What was checked, for the summary line: records, fields set to their own value, symbols, and
// lookups of a name in another scope that defines it too; optional fields left out and written
// again and those a type keeps written, files with a real change and its undo, files whose field
// took two coalesced Sets, records pasted, snapshots, files refusing another's Apply, the rows and
// the records added (those the writer takes only once filled in, and the Adds a type's own rule
// refused), and the findings validate_file made.
size_t g_records = 0, g_sets = 0, g_symbols = 0, g_other_scopes = 0;
size_t g_presences = 0, g_kept = 0, g_changes = 0, g_coalesced = 0, g_pastes = 0, g_snapshots = 0;
size_t g_foreign = 0, g_row_adds = 0, g_record_adds = 0, g_adds_waiting = 0, g_adds_refused = 0;
size_t g_findings = 0;
// S13 D7: files whose two rows took one batch and one gesture, and mixed batches taken and refused;
// text fields whose longer text grew their row's footprint.
size_t g_multi_rows = 0, g_mixed = 0, g_mixed_waiting = 0, g_mixed_refused = 0, g_grown = 0;
size_t g_checked_again = 0; // files a type's project check was brought to twice, and after a clear
// S13 D8: Record references kept naming their records across an Add before them, a Move and a Remove
// of what it added, the collections so renumbered, the Adds a type's own rule refused, and the
// Removes of a record a reference names that a type's own rule refused.
size_t g_record_references = 0, g_record_moves = 0, g_record_refused = 0, g_named_removes_refused = 0;
std::set<std::string> g_kinds; // each type's record kinds, by the type and the token
// Each type's record kinds over the files of every asset kind it opens: a kind's token by the kind.
std::map<std::string, std::map<NodeKind, std::string>> g_type_kinds;

// A type's optional fields over its files: those asked to be left out or written again, and those
// it did.
struct TypeCounts {
	size_t optional = 0, presences = 0;
	size_t findings = 0; // what validate_file made over the type's files
	size_t multi_rows = 0, mixed = 0, mixed_refused = 0; // S13 D7's batches over several rows
	size_t grown = 0; // text fields whose longer text grew their row's footprint
	size_t check_findings = 0; // what the type's project check made over them
	// S13 D8: the type's schema names a Record reference, and the references held across an Add.
	bool declares_records = false;
	size_t records = 0;
};

// Each row type and the size of its own object (S13 D7): a row's footprint is at least that. A row
// of a type this table does not name fails, so a new document type names its rows here.
struct RowObject {
	const std::type_info *type;
	size_t size;
};
const RowObject kRowObjects[] = {
        {&typeid(CatalogRow), sizeof(CatalogRow)}, {&typeid(StringsSection), sizeof(StringsSection)},
        {&typeid(MenuScreen), sizeof(MenuScreen)}, {&typeid(StyleRow), sizeof(StyleRow)},
        {&typeid(ModelRow), sizeof(ModelRow)},     {&typeid(CollisionRow), sizeof(CollisionRow)},
        {&typeid(ClipRow), sizeof(ClipRow)},       {&typeid(AnimationMapRow), sizeof(AnimationMapRow)},
        {&typeid(MissionRow), sizeof(MissionRow)}, {&typeid(EntityRow), sizeof(EntityRow)},
        {&typeid(PathRow), sizeof(PathRow)},       {&typeid(AreaRow), sizeof(AreaRow)},
        {&typeid(EventRow), sizeof(EventRow)},     {&typeid(EnvironmentRow), sizeof(EnvironmentRow)},
};
// The document types whose rows keep their text in fixed-length records (a model's 3DI records, a
// clip's bone table, a def catalog's records, a mission's header and entity slots): a longer text
// grows no row of theirs. Every other type's rows hold their text as strings, which a longer text
// makes longer.
const char *const kFixedText[] = {"model", "animation", "catalog", "mission"};
bool fixed_text(const DocumentType &type) {
	for (const char *name : kFixedText)
		if (std::string(name) == type.name) return true;
	return false;
}

size_t row_object(const Node &row) {
	for (const RowObject &entry : kRowObjects)
		if (*entry.type == typeid(row)) return entry.size;
	return 0;
}

// What each type's files ask of the presence clause and what the type does, as ADR 0046 S13 D2
// states them: every one left out and written again. A type not named has none. A change of a
// file above, or of a type's optional fields, moves them here and in the ADR together.
struct PinnedPresence {
	const char *type;
	size_t optional, presences;
};
const PinnedPresence kPinnedPresence[] = {{"menu", 301, 301}, {"catalog", 27, 27}, {"mission", 4, 4}, {"environment", 3, 3}};

// One clause of the contract, named with where it failed (the file, the record, the field).
void check(bool ok, const std::string &where, const char *clause) {
	if (ok) return;
	std::fprintf(stderr, "  %s: %s\n", where.c_str(), clause);
	++g_failures;
}

struct Fixture {
	AssetKind kind;
	std::string name;
	std::vector<uint8_t> bytes;
};

std::vector<uint8_t> text_bytes(const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); }

// The text types' files (S13 D9): a credits file in the CBIN form, laid out as the shipped one is,
// minted through the form's writer; a shader in the shader loader's SCR form; a music script with a
// message handler (which its MUS text has no form for), from the minted synth_gamemus.bin.
std::vector<uint8_t> credits_in_cbin() {
	using Config = opennova::cbin::BinaryConfig;
	Config config;
	config.strings = {"env", "text", "scroll_rate", "vertical_space", "~JC", "Opennova_Contract", "Serpen24", "<CR>"};
	config.xor_key = 0x0C0FFEE1u;
	const float rate = 0.5f;
	uint32_t rate_bits = 0;
	std::memcpy(&rate_bits, &rate, sizeof rate_bits);
	Config::Label env{1, {{3, {{rate_bits, Config::kFloat}}}, {4, {{14, Config::kInteger}}}}};
	Config::Label text{2, {{2, {{5, Config::kString}}}, {2, {{6, Config::kString}, {7, Config::kString}}},
	                       {2, {{8, Config::kString}}}}};
	config.labels = {env, text};
	std::vector<uint8_t> out;
	std::string error;
	opennova::cbin::encode_binary_config(config, out, error);
	return out;
}

std::vector<uint8_t> shader_in_scr(const std::string &text) {
	std::string payload = text + std::string(1, '\0');
	opennova::scr::scr_encrypt(reinterpret_cast<uint8_t *>(payload.data()), payload.size(),
	                           opennova::scr::SCR_KEY_SHADERS);
	return text_bytes(std::string("SCR\x01", 4) + payload);
}

std::vector<uint8_t> music_with_a_handler(const std::vector<uint8_t> &bin) {
	opennova::mus::MusFile file{};
	std::vector<uint8_t> out;
	if (bin.empty() || opennova::mus::mus_open_memory(&file, bin.data(), bin.size()) != 0) return out;
	file.scripts[0].has_message_handler = 1;
	const opennova::mus::MusScript *scripts[] = {&file.scripts[0]};
	uint8_t *buffer = nullptr;
	size_t size = 0;
	if (opennova::mus::mus_encode_file(scripts, 1, &buffer, &size) == 0) out.assign(buffer, buffer + size);
	opennova::mus::mus_free(buffer);
	opennova::mus::mus_close(&file);
	return out;
}

// The texture type's files (S18): a 4 x 4 image of graded alpha as a TGA and as an A8R8G8B8 DDS, and an
// 8-bit PCX.
std::vector<uint8_t> minted_rgba() {
	std::vector<uint8_t> rgba;
	for (int i = 0; i < 16; ++i)
		for (int c = 0; c < 4; ++c) rgba.push_back(uint8_t(i * 16 + c));
	return rgba;
}
std::vector<uint8_t> minted_tga() {
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(minted_rgba().data(), 4, 4, out, error);
	return out;
}
std::vector<uint8_t> minted_dds() {
	std::vector<uint8_t> out;
	std::string error;
	opennova::dds::dds_write_a8r8g8b8(minted_rgba().data(), 4, 4, out, error);
	return out;
}
std::vector<uint8_t> minted_pcx() {
	opennova::IndexedImage8 image;
	image.width = 4;
	image.height = 2;
	for (int i = 0; i < 8; ++i) image.indices.push_back(uint8_t(i * 3));
	for (int i = 0; i < 256; ++i) image.palette[i][0] = image.palette[i][1] = image.palette[i][2] = uint8_t(i);
	std::vector<uint8_t> out;
	std::string error;
	opennova::encode_pcx_indexed(image, out, error);
	return out;
}

// A menu screen whose root window holds an EXIT button: two of them give one window name in two
// scopes (a lookup on screen B must reach B's EXIT, never A's).
std::string exit_screen(const char *name, const char *action) {
	return std::string("<SCREEN>\n  <NAME>") + name +
	       "</NAME>\n  <WINDOW type=\"window\" name=\"ROOT\">\n"
	       "    <POSITION>\n      <LEFT>0</LEFT>\n      <TOP>0</TOP>\n"
	       "      <RIGHT>640</RIGHT>\n      <BOTTOM>480</BOTTOM>\n    </POSITION>\n"
	       "    <WINDOW type=\"button\" name=\"EXIT\">\n"
	       "      <POSITION>\n        <LEFT>20</LEFT>\n        <TOP>440</TOP>\n"
	       "        <RIGHT>120</RIGHT>\n        <BOTTOM>470</BOTTOM>\n      </POSITION>\n"
	       "      <STRING>Exit</STRING>\n      " +
	       action + "\n    </WINDOW>\n  </WINDOW>\n</SCREEN>\n";
}

// A file of every registered type's kinds: the repo's fixtures, a weapon, an ammo and a powerup
// table written here (the def fixture is an item table) with a record, a nested record and the
// written-unit, choice and reference fields the catalog shows, and a menu of two screens that
// each name a window EXIT.
std::vector<Fixture> fixtures(const std::string &repo) {
	const auto file = [&](const char *relative) { return test_io::read_file(repo + "/fixtures/" + relative); };
	return {
	        {AssetKind::ItemDefs, "items.def", file("def/items.def")},
	        {AssetKind::WeaponDefs, "weapon.def",
	         text_bytes("weapon \"WPN_CONTRACT\"\r\ncategory 11\r\nrank 3\r\nclipsize 30\r\nround_type AT_CONTRACT\r\n"
	                    "weaponweight 1.25\r\nerror_hiptheta 0.5\r\ncharfilter medic\r\nswitchcategory 3\r\nheat_sound SND_HEAT\r\n"
	                    "action \"FIRE\"\r\ndelayend 2\r\nend\r\nend\r\n")},
	        {AssetKind::AmmoDefs, "ammo.def",
	         text_bytes("ammo AT_CONTRACT\r\nmax_age 1.5\r\nvelocity 900\r\nturnrate_maxyaw 45\r\nlight_move 3 255 120 20\r\nend\r\n")},
	        // The worked example of a def family added as rows (S13 D10): a powerup with its ammo rows
	        // and both action blocks, the file's line ends CR LF as its reader cuts them.
	        {AssetKind::PowerupDefs, "powerup.def",
	         text_bytes("powerup \"PU_CONTRACT\"\r\nrespawn_time 30\r\nmax_respawns 2\r\nhp -1\r\nweapon WPN_CONTRACT\r\n"
	                    "ammo AT_CONTRACT 2\r\naction pickup\r\nfunction powerup_med\r\nsoundset SND_PICK\r\n"
	                    "delayend 10\r\nend\r\naction respawn\r\nparticle FX_BACK\r\nend\r\nend\r\n")},
	        {AssetKind::Strings, "synth_game.bin", file("rtxt/synth_game.bin")},
	        {AssetKind::Menu, "all_widgets.mnu", file("mnu/all_widgets.mnu")},
	        {AssetKind::Menu, "two_screens.mnu",
	         text_bytes(exit_screen("A", "<ACTION type=\"POP_SCREEN\"></ACTION>") + "\n" +
	                    exit_screen("B", "<ACTION type=\"SCREEN\" file=\"two_screens.mnu\">A</ACTION>"))},
	        {AssetKind::MenuStyle, "test_style.mns", file("mns/test_style.mns")},
	        {AssetKind::Model, "armory.3di", file("threedi/synth/armory.3di")},
	        {AssetKind::Animation, "walk.bad", file("anim/walk.bad")},
	        {AssetKind::AnimationMap, "soldier.adm", file("anim/soldier.adm")},
	        // The minted mission (S14): every row kind, a path, two events naming one another.
	        {AssetKind::Mission, "synth_logic.bms", file("bms/synth_logic.bms")},
	        // The text types (S13 D9).
	        {AssetKind::Script, "text_document.wac", file("wac/text_document.wac")},
	        {AssetKind::MusicScript, "gamemus.bin", file("mus/synth_gamemus.bin")},
	        {AssetKind::Credits, "nlist.kda", credits_in_cbin()},
	        {AssetKind::Shader, "glass.fx", shader_in_scr("// glass\r\nfloat4 main() : COLOR { return 0; }\r\n")},
	        {AssetKind::Config, "game.cfg", text_bytes("[Game]\r\nname = Contract\r\n")},
	        // The texture type (S18): a TGA, a PCX and a DDS, minted by our writers.
	        {AssetKind::Texture, "brick.tga", minted_tga()},
	        {AssetKind::Texture, "sky.pcx", minted_pcx()},
	        {AssetKind::Texture, "cube.dds", minted_dds()},
	        // The environment (DI-19a): the minted environment, every keyword and ten keyframes.
	        {AssetKind::Environment, "synth_full.env", file("env/synth_full.env")},
	};
}

// walk.bad's frames at 25 frames per second (every retail clip plays at 30): the clip's own
// finding (animation.fps). Empty when the clip does not read or write.
std::vector<uint8_t> clip_at_25fps(const std::vector<uint8_t> &walk) {
	opennova::bad::BadFile clip{};
	std::vector<uint8_t> out;
	if (walk.empty() || opennova::bad::bad_parse_buffer(walk.data(), walk.size(), &clip) != 0) return out;
	clip.fps = 25;
	if (opennova::bad::bad_write_buffer(&clip, out) != 0) out.clear();
	opennova::bad::bad_free(&clip);
	return out;
}

// A string table whose one section holds a key twice (strings.key_duplicate). Empty when the
// table does not write.
std::vector<uint8_t> table_with_a_key_twice() {
	opennova::rtxt::File table;
	table.sections.push_back({"menu", 2});
	table.entries.push_back({"KEY", "one", {}, 0});
	table.entries.push_back({"KEY", "two", {}, 0});
	std::vector<uint8_t> out;
	std::string error;
	if (!opennova::rtxt::write(table, out, error)) out.clear();
	return out;
}

// The minted mission with its two events' trigger runs laid out in the other order (the second
// event's trigger first in the table): what the game reads alike and Save lays out again
// (mission.event_order). Empty when the mission does not read or write.
std::vector<uint8_t> mission_with_runs_reordered(const std::vector<uint8_t> &minted) {
	opennova::bms::File file;
	std::vector<uint8_t> out;
	std::string error;
	if (minted.empty() || !opennova::bms::parse(minted.data(), minted.size(), file, error)) return out;
	if (file.events.size() != 2 || file.triggers.size() != 2) return out;
	std::swap(file.triggers[0], file.triggers[1]);
	file.events[0].trigger_index = 1;
	file.events[1].trigger_index = 0;
	if (!opennova::bms::write(file, out, error)) out.clear();
	return out;
}

// A 2 x 2 true-colour TGA whose header says its first row is the top one.
std::vector<uint8_t> top_first_tga() {
	std::vector<uint8_t> out(18, 0);
	out[2] = 2; // true colour
	out[12] = 2;
	out[14] = 2;
	out[16] = 32;
	out[17] = 0x28; // 8 alpha bits, the first row the top one
	out.resize(out.size() + 2 * 2 * 4, 0xFF);
	return out;
}

// A file of each type whose fixture above makes no finding, holding a flaw the type's
// validate_file reports (a key twice in a section, two screens of one NAME, a CTRL register the
// engine does not know, a clip at 25 frames per second, a slot named twice, a mission's runs out of
// its events' order, a TGA whose header says its rows run top first): what the per-type findings clause
// reads with the fixtures, through check_validate_file alone.
std::vector<Fixture> flawed_files(const std::string &repo) {
	const auto file = [&](const char *relative) { return test_io::read_file(repo + "/fixtures/" + relative); };
	const std::string pop = "<ACTION type=\"POP_SCREEN\"></ACTION>";
	return {
	        {AssetKind::Strings, "key_twice.bin", table_with_a_key_twice()},
	        {AssetKind::Menu, "twin_screens.mnu", text_bytes(exit_screen("A", pop.c_str()) + "\n" + exit_screen("A", pop.c_str()))},
	        {AssetKind::Model, "mount_ctrl1_not_retail.3di", file("threedi/synth/mount_ctrl1_not_retail.3di")},
	        {AssetKind::Animation, "walk_25fps.bad", clip_at_25fps(file("anim/walk.bad"))},
	        {AssetKind::AnimationMap, "slot_twice.adm",
	         text_bytes("anim_reset\t\"idle.bad\"\r\nanim_idle\t\"idle.bad\"\r\nanim_idle\t\"walk.bad\"\r\n")},
	        {AssetKind::Mission, "reordered.bms", mission_with_runs_reordered(file("bms/synth_logic.bms"))},
	        // The text types (S13 D9): a compile error, a message handler, credits lines holding spaces
	        // (the minted synth_nlist.kda), a plain shader.
	        {AssetKind::Script, "flawed.wac", text_bytes("fxrain FX_Buildup )\r\n")},
	        {AssetKind::MusicScript, "handled.bin", music_with_a_handler(file("mus/synth_gamemus.bin"))},
	        {AssetKind::Credits, "spaced.kda", file("cbin/synth_nlist.kda")},
	        {AssetKind::Shader, "plain.fx", text_bytes("float4 main() : COLOR { return 0; }\r\n")},
	        // A particle file the engine's reader stops in, which the text type holds (DI-06).
	        {AssetKind::Particles, "open.ptl", text_bytes("[effectdef]\n{\n\tid = OPEN;\n")},
	        // A 2 x 2 true-colour TGA, its origin bit set (S18: texture.tga_upside_down).
	        {AssetKind::Texture, "top_first.tga", top_first_tga()},
	        // An environment with a line the game skips and no sky height (DI-19a: environment.ignored_input,
	        // environment.sky_height_default).
	        {AssetKind::Environment, "flat_sky.env", text_bytes("fog_level 600\r\nspeling 3\r\n")},
	};
}

// Every registered document type, once each, in the order the asset kinds are declared.
std::vector<const DocumentType *> registered_types() {
	std::vector<const DocumentType *> out;
	for (size_t i = 1; i < kAssetKindCount; ++i) {
		const DocumentType *type = document_type_for(static_cast<AssetKind>(i));
		if (type && std::find(out.begin(), out.end(), type) == out.end()) out.push_back(type);
	}
	return out;
}

// Every record: each row, then what it holds in pre-order.
std::vector<NodeAddress> every_record(const Document &document) {
	std::vector<NodeAddress> out;
	for (const auto &row : document.rows()) {
		out.push_back({row->id, row->kind, 0});
		document.walk_records(*row, [&](const NodeAddress &nested, const Document::Placement &) {
			out.push_back(nested);
			return true;
		});
	}
	return out;
}

std::string where_of(const Fixture &fixture, const Document &document, const NodeAddress &address,
                     const std::string &field) {
	std::string out = fixture.name + " " + document.record_path(address);
	if (!field.empty()) out += " ." + field;
	return out;
}

// The reference_kinds row of a kind: its own, with a token that names it back.
bool has_row(ReferenceKind kind) {
	const ReferenceKindRow &row = reference_row(kind);
	ReferenceKind named = ReferenceKind::None;
	return row.kind == kind && *row.token && *row.label && reference_kind_from_token(row.token, named) && named == kind;
}

// The name a defining field's value gives (a text, or a number other than 0), as the graph's
// extraction reads it.
bool defines_a_name(const Value &value) {
	if (const auto *text = std::get_if<std::string>(&value)) return !text->empty();
	if (const auto *number = std::get_if<int64_t>(&value)) return *number != 0;
	return false;
}

// Whether a field carries the label the editor names it by. The one field that may go without:
// a def member its line does not write as a number of its own, which keeps the def table's id
// (the native member's name, the tooltip saying the key its file writes where the two differ);
// a member written as a number of its own is labelled by its line (def_table.cpp's
// describe: the table's name for it, else the key). field_title's fallback to the id is not a
// label.
bool labelled(const Document &document, const NodeAddress &address, const FieldSchema &field) {
	if (!field.label.empty()) return true;
	return dynamic_cast<const DefCatalogDocument *>(&document) &&
	       opennova::def::def_member(def_kind(address.kind), field.id).authored == opennova::def::DefAuthored::None;
}

// A record's field schema checks, as the record's field_on gives it.
void check_schema(const Fixture &fixture, const Document &document, const NodeAddress &address,
                  const FieldSchema &schema) {
	const std::string where = where_of(fixture, document, address, schema.id);
	const FieldUse field = document.field_on(address, schema);
	check(labelled(document, address, schema), where, "the field carries the label the editor names it by");
	Value value;
	check(document.get(address, schema.id, value), where, "the field reads");
	check(field.schema == &schema, where, "field_on points at the field's own schema (its id and type)");
	check(!schema.read_only || field.read_only, where, "field_on never makes a read-only field writable");
	if (schema.ranged)
		check(schema.type != FieldType::Text && schema.min <= schema.max && schema.step >= 0.0, where,
		      "a ranged field is a number whose range runs low to high");
	if (field.reference != ReferenceKind::None)
		check(has_row(field.reference), where, "the kind it references has its reference_kinds row");
	if (field.defines != ReferenceKind::None)
		check(has_row(field.defines), where, "the kind it defines has its reference_kinds row");
}

// Each writable field set to the value it reads: the serialized bytes stay as they were (the
// step, if the document takes one, undone after). A read-only field takes no set; one that
// does not read is check_schema's failure.
void check_set_to_self(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                       const std::string &serialized) {
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			Value value;
			if (field.read_only || !document.get(address, schema.id, value)) continue;
			const std::string where = where_of(fixture, document, address, schema.id);
			Edit edit;
			edit.operation = EditOperation::Set;
			edit.address = address;
			edit.field = schema.id;
			edit.value = value;
			Diagnostic error;
			const bool applied = document.apply(edit, error);
			check(applied, where + " (" + error.message + ")", "a writable field takes the value it reads");
			if (!applied) continue;
			++g_sets;
			const SerializeResult now = document.serialize();
			check(now.ok() && now.text == serialized, where, "a field set to the value it reads changes no byte");
			while (document.can_undo()) document.undo();
		}
}

// A scope no definition names: a section of the symbol's own file, or a file, of that name.
const char *const kNoScope = "CONTRACT_NO_SUCH_SCOPE";

// Every field a record defines a name by reads and yields the record's symbol: its locator names
// the record. find resolves the name in a scope only to a definition of the symbol's kind and
// name that a lookup reaches there: in its own scope this record, or the one of its name there a
// lookup finds first; in another scope of that kind's definitions the one there, or nothing where
// none is; in a scope no definition names nothing. A definition no lookup finds is still found,
// by its name alone, to a definition of its kind and name. Each find answers alike over the
// document alone and over a graph whose slot is current for it (the document's snapshot open in a
// graph of its one file), which it then reads the definitions from.
void check_symbols(const Fixture &fixture, const Document &document, const std::vector<NodeAddress> &records) {
	Extracted extracted;
	extract_from_document(document, extracted);
	AssetGraph slotted;
	AssetScan scan;
	AssetEntry entry;
	entry.logical_name = fixture.name;
	entry.relative_path = document.path();
	entry.kind = document.kind();
	scan.entries.push_back(entry);
	scan.index();
	slotted.update(ProjectPaths::for_root("."), ProjectDocument(), scan,
			{ std::shared_ptr<const DocumentBase>(document.snapshot()) });
	check(slotted.for_each_definition(document, [](const GraphSymbol &, bool) {}), fixture.name,
	      "a graph holding the document's snapshot has a slot current for the document");
	const auto find = [&](const std::string &where, const std::string &name, NodeAddress &found,
	                      const std::string &scope) {
		NodeAddress by_slot;
		const bool alone = find_definition(AssetGraph(), document, name, found, scope);
		const bool read = find_definition(slotted, document, name, by_slot, scope);
		check(read == alone && (!alone || by_slot == found), where,
				"find answers alike over the graph's slot");
		return alone;
	};
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			if (field.defines == ReferenceKind::None || field.applies == Applicability::Ignored ||
			    !document.present(address, schema.id))
				continue;
			const std::string where = where_of(fixture, document, address, schema.id);
			Value value;
			const bool read = document.get(address, schema.id, value);
			check(read, where, "a field that defines a name reads");
			if (!read || !defines_a_name(value)) continue;
			const auto symbol = std::find_if(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &s) {
				return s.address == address && s.field == schema.id;
			});
			check(symbol != extracted.symbols.end() && symbol->kind == field.defines, where,
			      "a field that defines a name yields its symbol");
		}
	const auto defined_as = [&](const GraphSymbol &symbol, const NodeAddress &found, bool reached, const std::string &scope) {
		return std::any_of(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &other) {
			return other.address == found && other.kind == symbol.kind && other.name == symbol.name &&
			       (!reached || (!other.inert && scope_matches(other.scope, scope)));
		});
	};
	for (const GraphSymbol &symbol : extracted.symbols) {
		++g_symbols;
		const std::string where = where_of(fixture, document, symbol.address, symbol.field) + " '" + symbol.display + "'";
		check(symbol.file == document.path() && symbol.locator == document.locator(symbol.address) &&
		              document.address_at(symbol.locator) == symbol.address,
		      where, "a symbol's locator names its defining record");
		// A record of a record set goes by its index in its own file, no name find looks up
		// (check_record_references holds it).
		if (reference_row(symbol.kind).resolution == ReferenceResolution::Record) continue;
		NodeAddress found;
		if (symbol.inert) {
			check(find(where, symbol.display, found, std::string()) &&
							defined_as(symbol, found, false, std::string()),
					where, "find resolves a name no lookup reaches by the name alone");
			continue;
		}
		// Its own scope, every other scope a definition of its kind has here, and one none has.
		std::vector<std::string> scopes{symbol.scope};
		for (const GraphSymbol &other : extracted.symbols)
			if (other.kind == symbol.kind && std::find(scopes.begin(), scopes.end(), other.scope) == scopes.end())
				scopes.push_back(other.scope);
		const size_t slash = symbol.scope.find('/');
		scopes.push_back((slash == std::string::npos ? std::string() : symbol.scope.substr(0, slash + 1)) + kNoScope);
		for (const std::string &scope : scopes) {
			const bool there = std::any_of(extracted.symbols.begin(), extracted.symbols.end(), [&](const GraphSymbol &other) {
				return !other.inert && other.kind == symbol.kind && other.name == symbol.name &&
				       scope_matches(other.scope, scope);
			});
			const std::string in = where + " in '" + scope + "'";
			const bool resolved = find(in, symbol.display, found, scope);
			if (!there) {
				check(!resolved, in, "find resolves a name in a scope that does not define it to nothing");
				continue;
			}
			if (scope == symbol.scope) {
				check(resolved && defined_as(symbol, found, true, scope), in,
				      "find resolves the symbol in its scope to its record, or the one of its name there first");
				continue;
			}
			++g_other_scopes;
			check(resolved && defined_as(symbol, found, true, scope), in,
			      "find resolves a name in another scope to the definition of its kind and name there");
		}
	}
}

Edit edit_of(EditOperation operation, const NodeAddress &address, const std::string &field, Value value = int64_t(0)) {
	Edit edit;
	edit.operation = operation;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

// Every record's locator finds it again, and a second load of the same bytes gives every record
// the identity the first gave it (a rename reloads a document and finds its records so).
// The type's record kinds (kinds()): each named back by its token and holding a label, no two
// sharing a kind or a token, a kind the outline adds a row of being a row of the file; every row
// of the file of a kind that is a row, and every record a collection holds of a kind the table
// has (its token the one its locator and a batch's add name it by).
void check_kinds(const DocumentType &type, const Fixture &fixture, const Document &document,
                 const std::vector<NodeAddress> &records) {
	const std::vector<RecordKindRow> &kinds = document.kinds();
	check(!kinds.empty(), fixture.name, "the type declares its record kinds");
	for (size_t i = 0; i < kinds.size(); ++i) {
		const RecordKindRow &row = kinds[i];
		const std::string where = fixture.name + " kind " + row.token;
		check(*row.token && *row.label && document.kind_from_name(row.token) == row.kind &&
		              document.kind_row(row.kind) == &row,
		      where, "a kind has a token and a label, and its token names it back");
		check(!*row.add_label || row.top, where, "a kind the outline adds is a row of the file");
		for (size_t j = 0; j < i; ++j)
			check(kinds[j].kind != row.kind && std::string(kinds[j].token) != row.token, where,
			      "no two kinds share a kind or a token");
		g_kinds.insert(std::string(typeid(document).name()) + "/" + row.token);
		const auto known = g_type_kinds[type.name].emplace(row.kind, row.token).first;
		check(known->second == row.token, where,
		      "a kind is one record kind across every asset kind the type opens (the same token)");
		check(type.fields && &type.fields(row.kind) == &document.fields(row.kind), where,
		      "the type's fields(kind) is the table its documents answer");
	}
	for (const NodeAddress &address : records) {
		const std::string where = where_of(fixture, document, address, "");
		const RecordKindRow *kind = document.kind_row(address.kind);
		check(kind && kind->top == (address.child == 0), where,
		      "a row of the file is of a kind that is a row, a nested record of one the table has");
		for (const Document::Collection &collection : document.collections_of(address))
			check(document.kind_row(collection.spec.kind) != nullptr, where,
			      "every kind a collection holds has its row");
	}
}

void check_places(const DocumentType &type, const Fixture &fixture, const Document &document,
                  const std::vector<NodeAddress> &records) {
	for (const NodeAddress &address : records)
		check(document.address_at(document.locator(address)) == address, where_of(fixture, document, address, ""),
		      "a record's locator finds it again");
	std::unique_ptr<Document> twin = records_of(type.make());
	Diagnostic error;
	const bool loaded = twin->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
	check(loaded && every_record(*twin) == records, fixture.name, "two loads of the file give the same identities");
}

// The one refusal a type gives a Clear or a Write of an optional field it keeps as its record
// writes it (a def line no tick of its own marks, a menu field no bit marks): always written.
bool always_written(const Diagnostic &error) {
	return error.code() == "document.value" &&
	       (error.message == "This field is always written." || error.message == "This line is always written.");
}

// Each writable optional field the game reads on its record left out and written again (Clear,
// Write), its latent value kept; a Clear of a field left out and a Write of one written no step;
// the undo giving the bytes back. A type may keep such a field written (always_written); any
// other refusal fails, and counts says how many the type was asked and how many it did.
void check_presence(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                    const std::string &serialized, TypeCounts &counts) {
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			if (!schema.optional || schema.read_only) continue;
			Value latent;
			if (document.field_on(address, schema).applies == Applicability::Ignored || !document.get(address, schema.id, latent))
				continue;
			const std::string where = where_of(fixture, document, address, schema.id);
			const bool written = document.present(address, schema.id);
			const uint64_t revision = document.revision();
			Diagnostic error;
			const bool nothing = document.apply(edit_of(written ? EditOperation::Write : EditOperation::Clear, address, schema.id), error);
			check(nothing && document.revision() == revision, where, "a Write of a written field, a Clear of one left out: no step");
			++counts.optional;
			error = Diagnostic();
			if (!document.apply(edit_of(written ? EditOperation::Clear : EditOperation::Write, address, schema.id), error)) {
				check(always_written(error), where + " (" + error.code() + ": " + error.message + ")",
				      "a type keeps an optional field as it is only as always written");
				++g_kept;
				continue;
			}
			++counts.presences;
			++g_presences;
			Value kept;
			check(document.present(address, schema.id) != written && document.get(address, schema.id, kept) && kept == latent,
			      where, "Clear leaves a field out and Write writes it again, its value kept");
			check(document.serialize().ok(), where, "a field left out or written again serializes");
			while (document.can_undo()) document.undo();
			check(document.present(address, schema.id) == written && document.serialize().text == serialized, where,
			      "the undo of a Clear or a Write gives the bytes back");
		}
}

// Values a field could take instead of `value`, the first ones its choices and range allow:
// another choice (a flags field's value with another bit), a number one or two either side, a
// text with a letter more or one less.
std::vector<Value> alternatives(const FieldSchema &field, const std::vector<FieldChoice> &choices, const Value &value) {
	std::vector<Value> out;
	const auto add = [&](Value candidate) {
		if (!(candidate == value) && std::find(out.begin(), out.end(), candidate) == out.end()) out.push_back(std::move(candidate));
	};
	if (const auto *number = std::get_if<int64_t>(&value)) {
		for (const FieldChoice &choice : choices) add(field.flags ? int64_t(*number ^ choice.value) : choice.value);
		double lo = field.ranged ? field.min : -1.0e15, hi = field.ranged ? field.max : 1.0e15;
		if (field.type != FieldType::Integer) lo = std::max(lo, 0.0);
		if (field.type == FieldType::Byte) hi = std::min(hi, 255.0);
		for (const int64_t step : {1, 2, -1, -2})
			if (double(*number + step) >= lo && double(*number + step) <= hi) add(int64_t(*number + step));
	} else if (const auto *real = std::get_if<double>(&value)) {
		for (const double step : {1.0, 2.0, -1.0, -2.0})
			if (!field.ranged || (*real + step >= field.min && *real + step <= field.max)) add(*real + step);
	} else if (const auto *text = std::get_if<std::string>(&value)) {
		for (const FieldChoice &choice : choices)
			if (!opennova::strutil::iequals(choice.name, *text)) add(choice.name);
		if (!field.width || text->size() + 3 <= field.width) {
			add(*text + "Q");
			add(*text + "QZ");
		}
		if (!text->empty()) add(text->substr(0, text->size() - 1));
	}
	return out;
}

// The fields a real change may be tried on, as they apply: writable, read by the game there,
// written, reading a value; with the values they could take instead.
template <class Try> void each_alternative(Document &document, const std::vector<NodeAddress> &records, Try try_one) {
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			const FieldUse field = document.field_on(address, schema);
			Value value;
			if (field.read_only || field.applies == Applicability::Ignored ||
			    (schema.optional && !document.present(address, schema.id)) || !document.get(address, schema.id, value))
				continue;
			std::vector<FieldChoice> own;
			if (try_one(address, schema, alternatives(schema, document.choices_on(address, field, own), value))) return;
		}
}

// The def members whose Set changes another member of their record for good (def_sync_derived,
// def_schema.cpp): an item's powerup_def and default_aip set a bit of its attrib, an action's
// function_args_count clears the arguments past it, a weapon's filter counts the names past
// them. revert_edits gives the member back alone, so the other one stays as the Set made it: an
// open item each (ADR 0046 S13 D2), and the one reason a revert here is not held to the file's
// bytes. (An item's armor_kz also sets armor_blast, a mirror its armor line writes equal, so its
// revert gives the bytes back.)
bool derives_others(const Document &document, const NodeAddress &address, const std::string &field) {
	using opennova::def::DefRecordKind;
	static const std::pair<DefRecordKind, const char *> kCoupled[] = {
	        {DefRecordKind::Item, "powerup_def"},        {DefRecordKind::Item, "default_aip"},
	        {DefRecordKind::Action, "function_args_count"}, {DefRecordKind::Weapon, "charfilter_count"},
	        {DefRecordKind::Weapon, "teamfilter_count"}};
	if (!dynamic_cast<const DefCatalogDocument *>(&document)) return false;
	return std::any_of(std::begin(kCoupled), std::end(kCoupled), [&](const std::pair<DefRecordKind, const char *> &member) {
		return member.first == def_kind(address.kind) && field == member.second;
	});
}

// A real change of one value: the bytes before, after, after its undo and after its redo; the
// record and the field changed since the save and no record of another row; revert_edits giving
// the field its saved value back as one step, and the record and the bytes as the file held them.
// A member that changes another of its record's (derives_others) is checked but for those bytes,
// and the clause goes on to the next field, so every file reaches the bytes of a revert.
void check_real_change(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                       const std::string &serialized) {
	bool done = false, given_back = false;
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		const bool coupled = derives_others(document, address, schema.id);
		for (const Value &option : options) {
			Diagnostic error;
			if (!document.apply(edit_of(EditOperation::Set, address, schema.id, option), error)) continue;
			const SerializeResult after = document.serialize();
			if (!after.ok() || after.text == serialized) {
				while (document.can_undo()) document.undo();
				continue;
			}
			const std::string where = where_of(fixture, document, address, schema.id);
			done = true;
			++g_changes;
			check(document.dirty() && document.can_undo(), where, "a real change is a step");
			check(document.field_changed(address, schema.id) && document.record_change(address) == Document::RecordChange::Changed,
			      where, "the field and its record changed since the save");
			for (const NodeAddress &other : records)
				if (other.row != address.row)
					check(document.record_change(other) == Document::RecordChange::Unchanged, where_of(fixture, document, other, ""),
					      "a record of another row is unchanged");
			const std::vector<Edit> back = document.revert_edits(address, schema.id);
			Value saved, reverted;
			check(!back.empty() && document.apply(back, error) && !document.field_changed(address, schema.id) &&
			              document.saved_value(address, schema.id, saved) && document.get(address, schema.id, reverted) &&
			              reverted == saved,
			      where, "revert_edits gives the field its saved value back");
			if (!coupled) {
				given_back = true;
				check(document.record_change(address) == Document::RecordChange::Unchanged &&
				              document.serialize().text == serialized,
				      where, "a field given back leaves its record and the bytes as the file held them");
			}
			document.undo();
			check(document.serialize().text == after.text, where, "the revert is one step");
			document.undo();
			check(document.serialize().text == serialized && !document.dirty(), where, "the undo of a change gives the bytes back");
			document.redo();
			check(document.serialize().text == after.text, where, "its redo gives the change back");
			while (document.can_undo()) document.undo();
			return !coupled;
		}
		return false;
	});
	check(done, fixture.name, "a writable field takes another value");
	check(given_back, fixture.name, "a field given back is held to the bytes the file held");
}

// Two coalesced Sets of one field (typing): one undo step.
void check_coalescing(const Fixture &fixture, Document &document, const std::vector<NodeAddress> &records,
                      const std::string &serialized) {
	bool done = false;
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		std::vector<Value> taken;
		for (const Value &option : options) {
			Edit set = edit_of(EditOperation::Set, address, schema.id, option);
			set.coalesce = true;
			Diagnostic error;
			if (!document.apply(set, error)) continue;
			taken.push_back(option);
			if (taken.size() == 2) break;
		}
		const bool two = taken.size() == 2 && document.serialize().text != serialized;
		if (two) {
			document.undo();
			check(document.serialize().text == serialized && !document.can_undo(),
			      where_of(fixture, document, address, schema.id), "two coalesced Sets of a field are one step");
			done = true;
			++g_coalesced;
		}
		while (document.can_undo()) document.undo();
		return two;
	});
	check(done, fixture.name, "a field takes two coalesced Sets");
}

// A record copied (where the type copies records) and pasted after its own: it is there, of its
// kind, the text reads back, and the undo gives the bytes back.
void check_copy_paste(const DocumentType &type, const Fixture &fixture, Document &document,
                      const std::vector<NodeAddress> &records, const std::string &serialized) {
	for (const NodeAddress &address : records) {
		if (!address.child) continue;
		const std::string payload = document.copy({address});
		Document::Placement at;
		if (payload.empty() || !document.placement(address, at)) continue;
		const std::string where = where_of(fixture, document, address, "");
		Edit paste = edit_of(EditOperation::Paste, {address.row, address.kind, 0}, std::string(), payload);
		paste.parent = at.owner.child;
		Diagnostic error;
		const bool pasted = document.apply(paste, error);
		check(pasted, where + " (" + error.message + ")", "a record copied pastes into its owner");
		if (!pasted) return;
		++g_pastes;
		const std::vector<NodeId> made = document.last_added_records();
		check(!made.empty() && document.address_of(made.front()).kind == address.kind &&
		              every_record(document).size() > records.size(),
		      where, "the pasted record is there, of its kind");
		const SerializeResult text = document.serialize();
		std::unique_ptr<Document> read = records_of(type.make());
		check(text.ok() && read->load_bytes(text_bytes(text.text), fixture.name, fixture.kind, "jo", error) && !read->blocked(),
		      where, "what a paste makes serializes and reads back");
		while (document.can_undo()) document.undo();
		check(document.serialize().text == serialized && !document.address_of(made.front()).row, where,
		      "the undo of a paste gives the bytes back");
		return;
	}
}

// A snapshot of the document with a real change in it: it serializes the document's bytes,
// shares its identity, revision, rows and records, answers record_change and field_changed as it
// does for every record and field, and refuses an edit, a save and a load (document.snapshot), its
// undo and redo doing nothing; after the document's own undo it serializes what it did, finds
// every record where it was (address_of) and answers what changed in each as it did.
void check_snapshot(const DocumentType &type, const Fixture &fixture, Document &document,
                    const std::vector<NodeAddress> &records, const std::string &serialized) {
	bool done = false;
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		for (const Value &option : options) {
			Diagnostic error;
			if (!document.apply(edit_of(EditOperation::Set, address, schema.id, option), error)) continue;
			const SerializeResult after = document.serialize();
			if (!after.ok() || after.text == serialized) {
				while (document.can_undo()) document.undo();
				continue;
			}
			done = true;
			const std::string where = where_of(fixture, document, address, schema.id) + " (snapshot)";
			const std::unique_ptr<Document> snapshot = records_of(document.snapshot());
			check(snapshot && snapshot->is_snapshot() && !document.is_snapshot(), where, "snapshot() makes a snapshot");
			if (!snapshot) {
				while (document.can_undo()) document.undo();
				return true;
			}
			++g_snapshots;
			const std::vector<NodeAddress> now = every_record(document);
			check(snapshot->serialize().text == after.text, where, "a snapshot serializes the document's bytes");
			check(snapshot->identity() == document.identity() && snapshot->revision() == document.revision() &&
			              snapshot->load_generation() == document.load_generation() &&
			              snapshot->rows() == document.rows() && every_record(*snapshot) == now,
			      where, "a snapshot shares the document's identity, revision, rows and records");
			// Its answers, kept to hold it to after its document moves on.
			std::vector<Document::RecordChange> records_changed;
			std::vector<bool> fields_changed;
			for (const NodeAddress &record : now) {
				records_changed.push_back(snapshot->record_change(record));
				check(records_changed.back() == document.record_change(record), where_of(fixture, document, record, ""),
				      "a snapshot answers record_change as its document does");
				for (const FieldSchema &field : document.fields(record.kind)) {
					fields_changed.push_back(snapshot->field_changed(record, field.id));
					check(fields_changed.back() == document.field_changed(record, field.id),
					      where_of(fixture, document, record, field.id), "a snapshot answers field_changed as its document does");
				}
			}
			Value saved;
			check(document.saved_value(address, schema.id, saved), where, "the changed field has its saved value");
			Diagnostic refused;
			check(!snapshot->apply(edit_of(EditOperation::Set, address, schema.id, saved), refused) &&
			              refused.code() == "document.snapshot",
			      where, "a snapshot refuses an edit (document.snapshot)");
			snapshot->undo();
			snapshot->redo();
			refused = Diagnostic();
			check(!snapshot->save(refused) && refused.code() == "document.snapshot", where,
			      "a snapshot refuses a save (document.snapshot)");
			refused = Diagnostic();
			check(!snapshot->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", refused) &&
			              refused.code() == "document.snapshot",
			      where, "a snapshot refuses a load (document.snapshot)");
			check(snapshot->revision() == document.revision() && snapshot->serialize().text == after.text &&
			              document.serialize().text == after.text,
			      where, "a snapshot's undo, redo and refusals leave it and its document as they were");
			while (document.can_undo()) document.undo();
			check(document.serialize().text == serialized && snapshot->serialize().text == after.text, where,
			      "the document's undo leaves its snapshot's bytes as they were");
			size_t field_at = 0;
			for (size_t i = 0; i < now.size(); ++i) {
				const NodeAddress &record = now[i];
				const std::string there = where_of(fixture, *snapshot, record, "") + " (snapshot, its document undone)";
				check(snapshot->address_of(record.child ? record.child : record.row) == record, there,
				      "a snapshot finds each record where it was after its document's undo");
				check(snapshot->record_change(record) == records_changed[i], there,
				      "a snapshot answers record_change as it did after its document's undo");
				for (const FieldSchema &field : snapshot->fields(record.kind))
					check(snapshot->field_changed(record, field.id) == fields_changed[field_at++], there + " ." + field.id,
					      "a snapshot answers field_changed as it did after its document's undo");
			}
			std::unique_ptr<Document> read = records_of(type.make());
			check(read->load_bytes(text_bytes(snapshot->serialize().text), fixture.name, fixture.kind, "jo", refused) &&
			              !read->blocked() && read->serialize().text == after.text,
			      where, "what a snapshot serializes reads back");
			return true;
		}
		return false;
	});
	check(done, fixture.name, "a document with a real change in it takes a snapshot");
}

// The type's validate_file (S13 D4): the file's own findings from its document alone, each on the
// file and, where it names a record, on one the document holds; a second load of the file, which
// gives its records the same identities, validates to the same findings.
void check_validate_file(const DocumentType &type, const Fixture &fixture,
		const DocumentBase &document, TypeCounts &counts) {
	const std::vector<Diagnostic> findings = type.validate_file(document);
	const Document *records = records_of(document);
	for (const Diagnostic &d : findings) {
		const std::string where = fixture.name + " " + d.code();
		check(d.asset == document.path(), where,
				"validate_file's findings are on the document's file");
		if (d.row_id)
			check(records && records->address_of(d.child_id ? d.child_id : d.row_id).row == d.row_id, where,
					"a finding names a record the document holds");
	}
	std::unique_ptr<DocumentBase> twin = type.make();
	Diagnostic error;
	check(twin->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error) &&
					type.validate_file(*twin) == findings,
			fixture.name, "a second load of the file validates to the same findings");
	g_findings += findings.size();
	counts.findings += findings.size();
}

// The type's project check (S13 V9), where it has one, over the file in a project of its own,
// validated first (a check reads which files' own checks read their records): a second update
// with nothing changed says nothing moved and keeps its findings, and clear() then an update makes
// the same findings again.
void check_project_check(const DocumentType &type, const Fixture &fixture, TypeCounts &counts) {
	if (!type.project_check) return;
	editor_test::TempProjectDir dir("opennova_editor_contract_project_check");
	const std::string root = dir.file("project");
	ProjectDocument project;
	Diagnostic error;
	const bool made = create_project(root, "Contract", "jo", project, error) &&
	                  editor_test::write_bytes(root + "/files/" + fixture.name, fixture.bytes);
	check(made, fixture.name + " (" + error.message + ")", "a project holding the file is made");
	if (!made) return;
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const AssetScan scan = scan_project_assets(paths, project);
	ProjectAssetSource files;
	files.set_scan(root, scan, project.target_game);
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const ValidationInput validation{paths, project, scan, open};
	refresh_project(validation, graph, cache);
	const ProjectCheckInput input{validation, cache, files};
	const std::unique_ptr<ProjectCheck> check_made = type.project_check();
	check(check_made != nullptr, fixture.name, "the type's hook makes its project check");
	if (!check_made) return;
	check_made->update(input);
	const std::vector<Diagnostic> first = check_made->findings();
	check(!check_made->update(input) && check_made->findings() == first, fixture.name,
	      "a second update of the project check with nothing changed says nothing moved and keeps its findings");
	check_made->clear();
	check_made->update(input);
	check(check_made->findings() == first, fixture.name,
	      "the project check cleared, then updated, makes the same findings again");
	++g_checked_again;
	counts.check_findings += first.size();
}

// A change of a kind no type makes (an Apply of another's payload): refused (document.payload),
// the document as it was.
struct ForeignPayload : EditPayload {
	const char *token() const override { return "contract.foreign"; }
};

void check_foreign_payload(const Fixture &fixture, Document &document,
                           const std::vector<NodeAddress> &records, const std::string &serialized) {
	Edit apply;
	apply.operation = EditOperation::Apply;
	apply.address = records.front();
	apply.payload = std::make_shared<ForeignPayload>();
	Diagnostic refused;
	const uint64_t revision = document.revision();
	++g_foreign;
	check(!document.apply(apply, refused) && refused.code() == "document.payload" &&
	              document.revision() == revision && !document.dirty() &&
	              document.serialize().text == serialized,
	      fixture.name,
	      "an Apply of a change the type did not make is refused (document.payload), nothing "
	      "committed");
}

// What the type declares it adds is added (make_node and edit_collection refuse by default, in the
// base's words): a row of each kind the outline adds (add_label) at the end, the last row and of
// its kind; a record into each kind of collection the file's records hold that is not fixed (each
// owner kind and record kind once, a full one skipped), of the collection's kind, or refused in
// the type's own words (a rule of its own: a model's LOD whose every part has its animation), and
// its Remove giving the owner the records it held. After each Add, what serializes reads back with
// the record kept; a new record the type's writer takes only once it is filled in (a catalog's
// action, sight, attachment or effect with its defaults, an animation table's row or clip naming
// none) is counted as waiting. Every Add's undo, and the Remove's, gives the bytes back.
void check_adds(const DocumentType &type, const Fixture &fixture, Document &document,
                const std::vector<NodeAddress> &records, const std::string &serialized) {
	const auto reads_back = [&](const std::string &where) {
		const SerializeResult text = document.serialize();
		if (!text.ok()) {
			++g_adds_waiting;
			return;
		}
		std::unique_ptr<Document> read = records_of(type.make());
		Diagnostic error;
		const bool loaded =
		        read->load_bytes(text_bytes(text.text), fixture.name, fixture.kind, "jo", error);
		check(loaded && !read->blocked() &&
		              every_record(*read).size() == every_record(document).size(),
		      where + " (" + error.message + ")", "what an Add makes reads back, the record kept");
	};
	for (const RecordKindRow &row : document.kinds()) {
		if (!*row.add_label) continue;
		const std::string where = fixture.name + " (" + row.add_label + ")";
		Diagnostic error;
		const bool added = document.apply(edit_of(EditOperation::Add, {0, row.kind, 0}, ""), error);
		check(added, where + " (" + error.message + ")",
		      "a row of a kind the outline adds is added (make_node)");
		if (!added) continue;
		++g_row_adds;
		// Where the type's order puts it (Document::row_position: a mission's band): the last row of
		// its kind, and the last of all for a type that keeps the position asked.
		const NodeId made = document.last_added();
		bool last_of_kind = false;
		for (const auto &each : document.rows()) {
			if (each->id == made) last_of_kind = each->kind == row.kind;
			else if (each->kind == row.kind) last_of_kind = false;
		}
		check(last_of_kind, where, "the row added is the last of its kind");
		reads_back(where);
		document.undo();
		check(document.serialize().text == serialized && !document.dirty(), where,
		      "an Add's undo gives the bytes back");
	}
	std::set<std::pair<NodeKind, NodeKind>> tried;
	for (const NodeAddress &owner : records) {
		for (const Document::Collection &collection : document.collections_of(owner)) {
			const bool full = collection.spec.max && collection.ids.size() >= collection.spec.max;
			if (collection.spec.fixed || full) continue;
			if (!tried.insert({owner.kind, collection.spec.kind}).second) continue;
			const std::string where = where_of(fixture, document, owner, "") + " (Add " +
			                          document.kind_token(collection.spec.kind) + ")";
			Edit add = edit_of(EditOperation::Add, {owner.row, collection.spec.kind, 0}, "");
			add.parent = owner.child;
			Diagnostic error;
			if (!document.apply(add, error)) {
				check(error.message != "This collection cannot accept that edit.",
				      where + " (" + error.message + ")",
				      "an Add into a collection that is not fixed is taken, or refused by a rule "
				      "of the type's own (edit_collection)");
				++g_adds_refused;
				continue;
			}
			++g_record_adds;
			const NodeAddress made = document.address_of(document.last_added());
			check(made.row && made.kind == collection.spec.kind, where,
			      "the record added is of the collection's kind");
			reads_back(where);
			const bool removed = document.apply(edit_of(EditOperation::Remove, made, ""), error);
			bool held = false;
			for (const Document::Collection &now : document.collections_of(owner))
				held = held || (now.spec.kind == collection.spec.kind && now.ids == collection.ids);
			check(removed && held, where,
			      "a Remove of the record added gives the owner the records it held");
			document.undo();
			if (removed) document.undo();
			check(document.serialize().text == serialized && !document.dirty(), where,
			      "the Add's and the Remove's undo give the bytes back");
		}
	}
}

// S13 D7: batches over several rows. A real change of each of two rows (the first Set of each that
// changes the bytes) in one batch: one step, the history holding its rows' bytes, what changed
// since the state before it exactly those two rows; undone to the bytes and redone to the bytes it
// made; the same two Sets as two batches of one gesture: one step. A batch mixing a record's Set
// with a row of each kind the outline adds, the last row duplicated and the last row moved to the
// top: one step, everything it made listed, undone and redone byte for byte; or refused by a rule
// of the type's own (a model's and a clip's fixed rows, a stylesheet's frozen lines), nothing
// committed.
void check_multi_row(const DocumentType &type, const Fixture &fixture, Document &document,
                     const std::vector<NodeAddress> &records, const std::string &serialized, TypeCounts &counts) {
	for (const auto &row : document.rows())
		check(row_object(*row) && row->footprint() >= row_object(*row),
		      where_of(fixture, document, {row->id, row->kind, 0}, ""),
		      "a row's footprint is at least its own object, of a row type the contract names");
	// A longer text grows its row's footprint by at least what it gained: each writable text field
	// of each record kind once, set to the longest text its width takes where that is longer than
	// what it holds (a field that refuses it, or keeps it otherwise, is passed over), undone after;
	// but for a type whose rows keep their text in fixed-length records (kFixedText).
	std::set<std::pair<NodeKind, std::string>> asked;
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document.fields(address.kind)) {
			if (fixed_text(type) || schema.type != FieldType::Text ||
			    !asked.insert({address.kind, schema.id}).second)
				continue;
			Value before;
			if (document.field_on(address, schema).read_only || !document.get(address, schema.id, before) ||
			    !std::holds_alternative<std::string>(before))
				continue;
			const std::string longer(schema.width ? schema.width - 1 : 4096, 'W');
			const size_t held = std::get<std::string>(before).size();
			if (longer.size() <= held) continue;
			const size_t was = document.row(address.row)->footprint();
			Diagnostic error;
			if (!document.apply(edit_of(EditOperation::Set, address, schema.id, longer), error)) continue;
			Value now;
			const bool kept = document.get(address, schema.id, now) && now == Value(longer);
			const size_t is = document.row(address.row)->footprint();
			while (document.can_undo()) document.undo();
			if (!kept) continue;
			check(is >= was + longer.size() - held, where_of(fixture, document, address, schema.id),
			      "a row whose text grows has a footprint larger by at least the text it gained");
			++g_grown;
			++counts.grown;
		}
	check(document.serialize().text == serialized && !document.dirty(), fixture.name,
	      "the longer texts undone give the bytes back");
	std::map<NodeId, Edit> real_sets; // the first Set of each row that changes the bytes, by row
	each_alternative(document, records, [&](const NodeAddress &address, const FieldSchema &schema,
	                                        const std::vector<Value> &options) {
		if (real_sets.count(address.row)) return false;
		for (const Value &option : options) {
			const Edit set = edit_of(EditOperation::Set, address, schema.id, option);
			Diagnostic error;
			if (!document.apply(set, error)) continue;
			const SerializeResult after = document.serialize();
			while (document.can_undo()) document.undo();
			if (!after.ok() || after.text == serialized) continue;
			real_sets.emplace(address.row, set);
			break;
		}
		return real_sets.size() >= 2;
	});
	if (real_sets.size() >= 2) {
		const Edit first = real_sets.begin()->second, second = std::next(real_sets.begin())->second;
		const std::string where = fixture.name + " (" + document.record_path(first.address) + " ." + first.field + ", " +
		                          document.record_path(second.address) + " ." + second.field + ")";
		const uint64_t load = document.load_generation(), revision = document.revision();
		Diagnostic error;
		check(document.apply({first, second}, error), where + " (" + error.message + ")", "a batch over two rows is taken");
		const std::string after = document.serialize().text;
		ChangeSet changes;
		const bool said = document.changes_since(load, revision, changes);
		const RowChanges *rows = said ? std::get_if<RowChanges>(&changes) : nullptr;
		check(rows && rows->changed == std::vector<NodeId>({first.address.row, second.address.row}) && rows->added.empty() &&
		              rows->removed.empty() && !rows->reordered,
		      where, "what changed since the batch's state before it is its two rows");
		check(document.history_bytes() > 0, where, "the history holds the step's rows");
		document.undo();
		check(document.serialize().text == serialized && !document.can_undo() && !document.dirty(), where,
		      "a batch over two rows is one step, undone to the bytes");
		document.redo();
		check(document.serialize().text == after, where, "and redone to the bytes it made");
		document.undo();
		Edit one = first, two = second;
		one.gesture = two.gesture = next_edit_gesture();
		check(document.apply(one, error) && document.apply(two, error) && document.serialize().text == after, where,
		      "a gesture's two batches over two rows are taken");
		document.undo();
		check(document.serialize().text == serialized && !document.can_undo(), where,
		      "a gesture over two rows is one step");
		++g_multi_rows;
		++counts.multi_rows;
	}

	std::vector<Edit> mixed;
	size_t makes = 0;
	if (!real_sets.empty()) mixed.push_back(real_sets.begin()->second);
	for (const RecordKindRow &row : document.kinds())
		if (*row.add_label) {
			mixed.push_back(edit_of(EditOperation::Add, {0, row.kind, 0}, ""));
			++makes;
		}
	if (document.rows().size() >= 2) {
		const Node &last = *document.rows().back();
		mixed.push_back(edit_of(EditOperation::Duplicate, {last.id, last.kind, 0}, ""));
		++makes;
		Edit move = edit_of(EditOperation::Move, {last.id, last.kind, 0}, "");
		move.position = 0;
		mixed.push_back(move);
	}
	if (mixed.size() < 2) return;
	const std::string where = fixture.name + " (a batch of rows and records)";
	const uint64_t revision = document.revision();
	Diagnostic error;
	if (!document.apply(mixed, error)) {
		check(document.revision() == revision && document.serialize().text == serialized && !document.dirty(), where,
		      "a refused batch commits nothing");
		check((error.code() == "document.structure" || error.code() == "document.kind" || error.code() == "document.collection") &&
		              error.message != "This document refuses that change." &&
		              error.message != "This document cannot add that record.",
		      where + " (" + error.code() + ": " + error.message + ")", "a batch of rows is refused only by a rule of the type's own");
		++g_mixed_refused;
		++counts.mixed_refused;
		return;
	}
	const SerializeResult after = document.serialize();
	check(document.last_added_records().size() == makes, where, "everything the batch made is listed");
	if (after.ok()) {
		std::unique_ptr<Document> read = records_of(type.make());
		Diagnostic unread;
		check(read->load_bytes(text_bytes(after.text), fixture.name, fixture.kind, "jo", unread) && !read->blocked(),
		      where + " (" + unread.message + ")", "what the batch makes reads back");
	} else {
		++g_mixed_waiting;
	}
	document.undo();
	check(document.serialize().text == serialized && !document.can_undo() && !document.dirty(), where,
	      "a batch of rows and records is one step, undone to the bytes");
	document.redo();
	if (after.ok()) check(document.serialize().text == after.text, where, "and redone to the bytes it made");
	document.undo();
	++g_mixed;
	++counts.mixed;
}

// A text type over its file (S13 D9): the lifecycle's clauses and D7's change sets, by spans.
size_t g_text_files = 0, g_text_changes = 0;

void check_text_fixture(const DocumentType &type, const Fixture &fixture, TypeCounts &counts) {
	std::unique_ptr<DocumentBase> made = type.make();
	check(made && made->as_text() == made.get() && text_of(*made) == made->as_text() && !records_of(*made),
	      fixture.name, "make gives a DocumentBase whose text document (as_text) is itself, holding no records");
	if (!made || !text_of(*made)) return;
	DocumentBase &document = *made;
	const TextDocument &text = *text_of(document);
	Diagnostic error;
	const bool loaded = document.load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
	check(loaded && !document.blocked(), fixture.name + " (" + error.message + ")", "the file loads unblocked");
	if (!loaded || document.blocked()) return;
	++g_text_files;
	const std::string stored(fixture.bytes.begin(), fixture.bytes.end());
	const SerializeResult first = document.serialize();
	check(first.ok() && first.text == stored && document.rewrite_need() == DocumentBase::RewriteNeed::None,
	      fixture.name, "a text document serializes the bytes it was read from");
	std::unique_ptr<DocumentBase> again = type.make();
	check(again->load_bytes(text_bytes(first.text), fixture.name, fixture.kind, "jo", error) &&
	              again->serialize().text == first.text && text_of(*again)->text() == text.text(),
	      fixture.name, "parse, serialize, parse again serializes the same bytes and text");
	check_validate_file(type, fixture, document, counts);
	// Another's change, or an edit naming a record: refused, nothing committed.
	Edit apply;
	apply.operation = EditOperation::Apply;
	apply.payload = std::make_shared<ForeignPayload>();
	Diagnostic refused;
	++g_foreign;
	check(!document.apply(apply, refused) && refused.code() == "document.payload" && document.revision() == 0 &&
	              !document.dirty(),
	      fixture.name, "an Apply of a change the type did not make is refused (document.payload)");
	Edit set;
	set.address = {1, 0, 0};
	set.field = "name";
	refused = Diagnostic();
	check(!document.apply(set, refused) && refused.code() == "document.payload" && document.revision() == 0, fixture.name,
	      "an edit naming a record is refused (document.payload)");
	// A real change: a span replaced whose bytes the type's writer writes otherwise (a digit made
	// another, the first that does it; else the first character).
	const uint64_t load = document.load_generation();
	TextSpan first_line;
	first_line.line = 1;
	first_line.column = 1;
	first_line.length = std::min<size_t>(text.line(1).size(), 1);
	std::vector<size_t> candidates;
	for (size_t i = 0; i < text.text().size() && candidates.size() < 64; ++i)
		if (text.text()[i] >= '0' && text.text()[i] <= '9') candidates.push_back(i);
	candidates.push_back(0);
	TextSpan real;
	SerializeResult changed;
	for (const size_t at : candidates) {
		const char held = at < text.text().size() ? text.text()[at] : ' ';
		const std::string with(1, held >= '0' && held <= '8' ? char(held + 1) : held == '9' ? '0' : 'Z');
		const TextSpan span = text.span_at(at, at < text.text().size() ? 1 : 0);
		if (!document.apply(TextDocument::replace(span, with), error)) continue;
		changed = document.serialize();
		if (changed.ok() && changed.text != stored) {
			real = span;
			break;
		}
		document.undo();
	}
	check(real.line != 0 && document.dirty() && document.can_undo() && document.history_bytes() > 0, fixture.name,
	      "a real change: a span replaced is a step whose bytes are other");
	if (real.line == 0) return;
	ChangeSet since;
	const TextChanges *spans = nullptr;
	check(document.changes_since(load, 0, since) && (spans = std::get_if<TextChanges>(&since)) &&
	              spans->spans.size() == 1 && spans->spans[0].line == real.line &&
	              spans->spans[0].column == real.column,
	      fixture.name, "what changed since the load is the span");
	document.undo();
	check(!document.dirty() && document.serialize().text == stored, fixture.name, "its undo gives the bytes back");
	document.redo();
	check(document.serialize().text == changed.text, fixture.name, "its redo the change");
	document.undo();
	++g_changes;
	++g_text_changes;
	// Two coalesced replacements one step; a gesture's two batches one step.
	const uint64_t before = document.revision();
	check(document.apply(TextDocument::replace(first_line, "Y", true), error) &&
	              document.apply(TextDocument::replace(first_line, "X", true), error),
	      fixture.name, "two coalesced replacements apply");
	document.undo();
	check(document.revision() == before && document.serialize().text == stored, fixture.name,
	      "two coalesced replacements are one step");
	++g_coalesced;
	document.end_edit_group();
	const uint64_t gesture = next_edit_gesture();
	const size_t last = text.line_count();
	TextSpan end_line;
	end_line.line = last;
	end_line.column = text.line(last).size() + 1;
	check(document.apply(TextDocument::replace(first_line, "W", false, gesture), error) &&
	              document.apply(TextDocument::replace(end_line, "V", false, gesture), error),
	      fixture.name, "a gesture's two batches apply");
	check(document.changes_since(load, before, since) && (spans = std::get_if<TextChanges>(&since)) &&
	              spans->spans.size() == (last == 1 ? 1u : 2u),
	      fixture.name, "a gesture's change set holds its spans");
	document.undo();
	check(!document.can_undo() && document.serialize().text == stored, fixture.name, "a gesture's two batches are one step");
	// A snapshot of a changed document.
	check(document.apply(TextDocument::replace(first_line, "U"), error), fixture.name, "a change to snapshot");
	const std::unique_ptr<DocumentBase> snapshot = document.snapshot();
	check(snapshot && snapshot->is_snapshot() && snapshot->identity() == document.identity() &&
	              snapshot->load_generation() == document.load_generation() &&
	              snapshot->revision() == document.revision() &&
	              snapshot->serialize().text == document.serialize().text,
	      fixture.name, "a snapshot shares the document's identity, load generation and revision, and its bytes");
	refused = Diagnostic();
	check(!snapshot->apply(TextDocument::replace(first_line, "T"), refused) && refused.code() == "document.snapshot",
	      fixture.name, "a snapshot refuses an edit (document.snapshot)");
	refused = Diagnostic();
	check(!snapshot->save(refused) && refused.code() == "document.snapshot", fixture.name,
	      "a snapshot refuses a save (document.snapshot)");
	refused = Diagnostic();
	check(!snapshot->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", refused) &&
	              refused.code() == "document.snapshot",
	      fixture.name, "a snapshot refuses a load (document.snapshot)");
	++g_snapshots;
	document.undo();
	check(document.serialize().text == stored, fixture.name, "the document's undo gives the bytes back");
}

// S13 D8: the Record references a type's schema names (Document::targeted_collections: an index
// into a collection of its own file). Every Record kind a field of the type names is one of its
// targeted collections (a collection token that names none of its record kinds would leave the
// references with no record set and no renumbering). What each reference names in the file's
// extraction: the record of its record set at its index (by the record's identity; none past the
// collection). For each collection a reference names, a record added where the first record named
// stands (the core has the type renumber what names the collection): one step, every reference
// naming the record it named; what serializes reads back with the record kept; the added record
// moved to the collection's end, every reference naming its record again; removed again, the
// same, the file's bytes back (renumbered the other way), and so with each undo. A type may refuse
// the Add by a rule of its own (document.collection in its own words), counted. And the first
// record named removed: refused by the type's own rule (a reference would name a record that is
// gone), nothing committed.
void check_record_references(const DocumentType &type, const Fixture &fixture, Document &document,
                             const std::string &serialized, TypeCounts &counts) {
	const std::vector<Document::TargetedCollection> targets = document.targeted_collections();
	for (const RecordKindRow &kind : document.kinds())
		for (const FieldSchema &field : document.fields(kind.kind)) {
			if (reference_row(field.reference).resolution != ReferenceResolution::Record) continue;
			bool targeted = false;
			for (const Document::TargetedCollection &target : targets)
				targeted = targeted || target.reference == field.reference;
			check(targeted, fixture.name + " (" + kind.token + " " + field.id + ")",
			      "a Record kind a field names is one of the type's targeted collections");
		}
	if (targets.empty()) return;
	counts.declares_records = true;
	// (the referring record, its field) -> the record its index names (0: none of the set).
	using Named = std::map<std::pair<NodeId, std::string>, NodeId>;
	const auto named = [&](ReferenceKind kind, NodeAddress *first) {
		Extracted extracted;
		extract_from_document(document, extracted);
		std::map<std::string, NodeAddress> set;
		for (const GraphSymbol &symbol : extracted.symbols)
			if (symbol.kind == kind) set[symbol.name] = symbol.address;
		Named out;
		size_t lowest = SIZE_MAX;
		for (const GraphEdge &edge : extracted.edges) {
			if (edge.kind != kind) continue;
			const auto found = set.find(edge.value);
			const NodeAddress record = found == set.end() ? NodeAddress() : found->second;
			out[{edge.address.child ? edge.address.child : edge.address.row, edge.field}] =
			        record.child ? record.child : record.row;
			const std::optional<int> index = opennova::strutil::parse_int(edge.value);
			if (record.row && index && size_t(*index) < lowest) {
				lowest = size_t(*index);
				if (first) *first = record;
			}
		}
		return out;
	};
	for (const Document::TargetedCollection &target : targets) {
		const std::string where = fixture.name + " (" + reference_row(target.reference).token + ")";
		NodeAddress first;
		const Named before = named(target.reference, &first);
		if (before.empty() || !first.row) continue;
		// The record is one a record holds (its placement there), or a row of the file (S14: a
		// mission's markers and events), which is added, moved and removed among the rows; rows the
		// outline adds none of (a mission's 128 paths) are a fixed table no edit renumbers.
		Document::Placement at;
		const bool row_level = first.child == 0;
		check((row_level || document.placement(first, at)) && first.kind == target.kind, where,
		      "a record a Record reference names is of its collection's kind");
		if (!row_level && !document.placement(first, at)) continue;
		if (row_level && !*document.kind_row(target.kind)->add_label) continue;
		Edit add = edit_of(EditOperation::Add, {row_level ? 0 : first.row, target.kind, 0}, "");
		add.parent = at.owner.child;
		add.position = at.index;
		if (row_level) {
			add.position = 0;
			for (const auto &each : document.rows()) {
				if (each->id == first.row) break;
				++add.position;
			}
		}
		Diagnostic error;
		if (!document.apply(add, error)) {
			check(error.code() == "document.collection" && error.message != "This collection cannot accept that edit.",
			      where + " (" + error.code() + ": " + error.message + ")",
			      "a record added before the records named is taken, or refused by a rule of the type's own");
			check(document.serialize().text == serialized && !document.dirty(), where, "a refused Add commits nothing");
			++g_record_refused;
			continue;
		}
		const NodeId added = document.last_added();
		check(document.can_undo() && named(target.reference, nullptr) == before, where,
		      "a record added before the records named leaves every reference naming the record it named");
		const SerializeResult text = document.serialize();
		std::unique_ptr<Document> read = records_of(type.make());
		check(text.ok() && read->load_bytes(text_bytes(text.text), fixture.name, fixture.kind, "jo", error) &&
		              every_record(*read).size() == every_record(document).size(),
		      where, "what the renumbering Add makes reads back");
		// The added record moved to the end of its collection: the records it passes move back.
		Edit move = edit_of(EditOperation::Move, document.address_of(added), "");
		move.parent = at.owner.child;
		move.position = SIZE_MAX;
		check(document.apply(move, error) && named(target.reference, nullptr) == before,
		      where + " (" + error.message + ")", "a Move of the added record leaves every reference naming its record");
		document.undo();
		check(document.serialize().text == text.text, where, "the Move is one step");
		check(document.apply(edit_of(EditOperation::Remove, document.address_of(added), ""), error) &&
		              named(target.reference, nullptr) == before && document.serialize().text == serialized,
		      where + " (" + error.message + ")",
		      "the record removed again leaves every reference naming its record, the bytes back");
		document.undo();
		check(document.serialize().text == text.text, where, "the Remove is one step");
		document.undo();
		check(document.serialize().text == serialized && !document.dirty(), where,
		      "the renumbering Add is one step, undone to the bytes");
		// The first record named removed: a reference would name a record that is gone, which the
		// type refuses by its own rule, nothing committed.
		const uint64_t revision = document.revision();
		check(!document.apply(edit_of(EditOperation::Remove, first, ""), error) &&
		              error.code() == "document.collection" &&
		              error.message != "This collection cannot accept that edit." &&
		              document.revision() == revision && document.serialize().text == serialized,
		      where + " (" + error.message + ")", "a record a reference names is not removed from under it");
		++g_named_removes_refused;
		g_record_references += before.size();
		counts.records += before.size();
		++g_record_moves;
	}
}

// An image type over its file (S18, a texture): make gives a DocumentBase that holds an image and
// neither records nor a text; the file loads unblocked and serializes the bytes it was read from (no
// rewrite), parse, serialize, parse again the same bytes; its validate_file's findings on its file
// alone, a second load validating to the same; a change of a kind the type did not make, or an edit
// naming a record, refused (document.payload) with nothing committed; nothing changed since its load
// (an empty change set of its own kind), another load's state not said; and a snapshot sharing its
// identity, load and revision, serializing its bytes and refusing an edit, a save and a load.
size_t g_image_files = 0;

void check_image_fixture(const DocumentType &type, const Fixture &fixture, TypeCounts &counts) {
	std::unique_ptr<DocumentBase> made = type.make();
	check(made && made->holds_image() && !records_of(*made) && !text_of(*made), fixture.name,
	      "make gives a DocumentBase that holds an image, neither records nor a text");
	if (!made || !made->holds_image()) return;
	DocumentBase &document = *made;
	Diagnostic error;
	const bool loaded = document.load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
	check(loaded && !document.blocked(), fixture.name + " (" + error.message + ")", "the file loads unblocked");
	if (!loaded || document.blocked()) return;
	++g_image_files;
	const std::string stored(fixture.bytes.begin(), fixture.bytes.end());
	const SerializeResult first = document.serialize();
	check(first.ok() && first.text == stored && document.rewrite_need() == DocumentBase::RewriteNeed::None, fixture.name,
	      "an image document serializes the bytes it was read from");
	std::unique_ptr<DocumentBase> again = type.make();
	check(again->load_bytes(text_bytes(first.text), fixture.name, fixture.kind, "jo", error) &&
	              again->serialize().text == first.text,
	      fixture.name, "parse, serialize, parse again serializes the same bytes");
	check_validate_file(type, fixture, document, counts);
	Edit apply;
	apply.operation = EditOperation::Apply;
	apply.payload = std::make_shared<ForeignPayload>();
	Diagnostic refused;
	++g_foreign;
	check(!document.apply(apply, refused) && refused.code() == "document.payload" && document.revision() == 0 &&
	              !document.dirty() && document.serialize().text == stored,
	      fixture.name, "an Apply of a change the type did not make is refused (document.payload)");
	Edit set;
	set.address = {1, 0, 0};
	set.field = "name";
	refused = Diagnostic();
	check(!document.apply(set, refused) && refused.code() == "document.payload" && document.revision() == 0, fixture.name,
	      "an edit naming a record is refused (document.payload)");
	ChangeSet since;
	check(document.changes_since(document.load_generation(), document.revision(), since) &&
	              std::get_if<RasterChanges>(&since) && std::get_if<RasterChanges>(&since)->regions.empty() &&
	              !document.changes_since(document.load_generation() + 1, 0, since),
	      fixture.name, "nothing changed since its load; another load's state is not said");
	const std::unique_ptr<DocumentBase> snapshot = document.snapshot();
	check(snapshot && snapshot->is_snapshot() && snapshot->identity() == document.identity() &&
	              snapshot->load_generation() == document.load_generation() &&
	              snapshot->revision() == document.revision() && snapshot->holds_image() &&
	              snapshot->serialize().text == stored,
	      fixture.name, "a snapshot shares the document's identity, load generation and revision, and its bytes");
	refused = Diagnostic();
	check(!snapshot->apply(apply, refused) && refused.code() == "document.snapshot", fixture.name,
	      "a snapshot refuses an edit (document.snapshot)");
	refused = Diagnostic();
	check(!snapshot->save(refused) && refused.code() == "document.snapshot", fixture.name,
	      "a snapshot refuses a save (document.snapshot)");
	refused = Diagnostic();
	check(!snapshot->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", refused) &&
	              refused.code() == "document.snapshot",
	      fixture.name, "a snapshot refuses a load (document.snapshot)");
	++g_snapshots;
}

void check_fixture(const DocumentType &type, const Fixture &fixture, TypeCounts &counts) {
	if (document_content(type) == DocumentContent::Text) return check_text_fixture(type, fixture, counts);
	if (document_content(type) == DocumentContent::Image) return check_image_fixture(type, fixture, counts);
	std::unique_ptr<DocumentBase> made = type.make();
	check(made && made->as_records() == made.get() && records_of(*made) == made->as_records(),
	      fixture.name, "make gives a DocumentBase whose record document (as_records) is itself");
	std::unique_ptr<Document> document = records_of(std::move(made));
	if (!document) return;
	Diagnostic error;
	const bool loaded = document->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
	check(loaded && !document->blocked(), fixture.name + " (" + error.message + ")", "the file loads unblocked");
	if (!loaded || document->blocked()) return;
	const std::vector<NodeAddress> records = every_record(*document);
	check(!records.empty(), fixture.name, "the file has records to check");
	g_records += records.size();

	// Parse, serialize, parse again: the same bytes, the same records.
	const SerializeResult first = document->serialize();
	check(first.ok(), fixture.name, "the file serializes");
	std::unique_ptr<Document> again = records_of(type.make());
	const bool reloaded = again->load_bytes(text_bytes(first.text), fixture.name, fixture.kind, "jo", error);
	check(reloaded && !again->blocked(), fixture.name + " (" + error.message + ")", "what it serializes loads unblocked");
	if (reloaded) {
		const SerializeResult second = again->serialize();
		check(second.ok() && second.text == first.text && every_record(*again).size() == records.size(), fixture.name,
		      "parse, serialize, parse again serializes the same bytes and records");
	}

	check_kinds(type, fixture, *document, records);
	check_validate_file(type, fixture, *document, counts);
	check_foreign_payload(fixture, *document, records, first.text);
	check_adds(type, fixture, *document, records, first.text);
	check_places(type, fixture, *document, records);
	for (const NodeAddress &address : records)
		for (const FieldSchema &schema : document->fields(address.kind)) check_schema(fixture, *document, address, schema);
	check_symbols(fixture, *document, records);
	check_set_to_self(fixture, *document, records, first.text);
	check_presence(fixture, *document, records, first.text, counts);
	check_real_change(fixture, *document, records, first.text);
	check_coalescing(fixture, *document, records, first.text);
	check_copy_paste(type, fixture, *document, records, first.text);
	check_snapshot(type, fixture, *document, records, first.text);
	check_multi_row(type, fixture, *document, records, first.text, counts);
	check_record_references(type, fixture, *document, first.text, counts);
}

} // namespace

int main() {
	const std::string repo = test_paths_repo_root(__FILE__);
	const std::vector<Fixture> files = fixtures(repo);
	const std::vector<Fixture> flawed = flawed_files(repo);
	const std::vector<const DocumentType *> types = registered_types();
	for (const DocumentType *type : types) {
		size_t checked = 0;
		TypeCounts counts;
		for (const Fixture &fixture : files) {
			if (document_type_for(fixture.kind) != type) continue;
			check(!fixture.bytes.empty(), fixture.name, "the fixture is present");
			if (fixture.bytes.empty()) continue;
			check_fixture(*type, fixture, counts);
			check_project_check(*type, fixture, counts);
			++checked;
		}
		check(checked > 0, type->name, "the document type has a file here to check");
		for (const Fixture &fixture : flawed) {
			if (document_type_for(fixture.kind) != type) continue;
			std::unique_ptr<DocumentBase> document = type->make();
			Diagnostic error;
			const bool loaded = document && !fixture.bytes.empty() &&
			                    document->load_bytes(fixture.bytes, fixture.name, fixture.kind, "jo", error);
			check(loaded, fixture.name + " (" + error.message + ")", "the flawed file is made and loads");
			if (loaded) check_validate_file(*type, fixture, *document, counts);
		}
		// Per type: a validate_file that never took its own documents (its cast to another type)
		// would make nothing over its files. The text type's are the engine readers' (DI-06).
		check(counts.findings > 0, type->name, "validate_file makes a finding over the type's files");
		// Likewise a project check that never read its type's files would keep the clause above
		// over no findings.
		check(!type->project_check || counts.check_findings > 0, type->name,
		      "the project check makes a finding over the type's files");
		PinnedPresence pinned{type->name, 0, 0};
		for (const PinnedPresence &pin : kPinnedPresence)
			if (std::string(pin.type) == type->name) pinned = pin;
		check(counts.optional == pinned.optional && counts.presences == pinned.presences, type->name,
		      "a type's optional fields asked and left out and written again are the ones pinned");
		check(fixed_text(*type) || document_content(*type) == DocumentContent::Text ||
		              document_content(*type) == DocumentContent::Image || counts.grown > 0,
		      type->name, "a longer text grows a row of the type");
		// S13 D8: a type whose schema names a Record reference renumbers one over its files.
		check(!counts.declares_records || counts.records > 0, type->name,
		      "a type whose records name others of their file by index keeps them named across an Add");
		std::printf("  %s: %zu optional fields asked, %zu left out and written again, %zu findings; %zu files' two "
		            "rows in one batch, %zu mixed batches taken, %zu refused by its rule; %zu text fields' "
		            "longer text grew their row",
		            type->name, counts.optional, counts.presences, counts.findings, counts.multi_rows, counts.mixed,
		            counts.mixed_refused, counts.grown);
		if (type->project_check) std::printf(", %zu project check findings", counts.check_findings);
		std::printf("\n");
	}
	for (const Fixture &fixture : files)
		check(document_type_for(fixture.kind) != nullptr, fixture.name, "the file is of a registered type");
	check(g_other_scopes > 0, "the files", "a name defined in two scopes is looked up in the other");
	check(g_presences > 0 && g_pastes > 0, "the files", "an optional field is left out and written, a record pasted");
	check(g_multi_rows > 0 && g_mixed > 0 && g_mixed_refused > 0, "the files",
	      "two rows change in one batch, a batch of rows and records is taken, and one refused by a type's rule");
	check(g_grown > 0, "the files", "a longer text grows its row's footprint");
	check(g_record_moves > 0 && g_named_removes_refused > 0, "the files",
	      "a collection other records name by index is renumbered, and a record named is never removed");
	check(g_image_files > 0, "the files", "an image type's file keeps the contract");
	std::printf("  %zu image documents' files\n", g_image_files);
	if (g_failures == 0)
		std::printf("editor_document_contract: all %zu document types keep the contract (%zu files, %zu records, "
		            "%zu fields set to their own value, %zu symbols, %zu lookups in another scope of the name, "
		            "%zu optional fields left out and written again, %zu kept always written, %zu real changes "
		            "undone and redone, %zu coalesced, %zu records pasted, %zu snapshots, %zu foreign "
		            "changes refused, %zu rows and %zu records added (%zu waiting for values, "
		            "%zu Adds refused by a type's rule), %zu record kinds, %zu findings validate_file made, %zu files' "
		            "two rows in one batch and one gesture, %zu batches of rows and records taken (%zu waiting for "
		            "values) and %zu refused by a type's rule, %zu text fields whose longer text grew their row, "
		            "%zu files a project check was brought to again, %zu Record references kept naming their "
		            "records across an Add, a Move and a Remove in %zu collections, %zu such Adds refused by a "
		            "type's rule, %zu Removes of a record named refused by its rule; %zu text documents' files, "
		            "%zu spans replaced, undone and redone)\n",
		            types.size(), files.size(), g_records, g_sets, g_symbols, g_other_scopes, g_presences, g_kept,
		            g_changes, g_coalesced, g_pastes, g_snapshots, g_foreign, g_row_adds,
		            g_record_adds, g_adds_waiting, g_adds_refused, g_kinds.size(), g_findings, g_multi_rows, g_mixed,
		            g_mixed_waiting, g_mixed_refused, g_grown, g_checked_again, g_record_references,
		            g_record_moves, g_record_refused, g_named_removes_refused, g_text_files, g_text_changes);
	return g_failures == 0 ? 0 : 1;
}
