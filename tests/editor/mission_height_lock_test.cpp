// The mission's Height lock (ADR 0046 S25, the original editor's): the entities' field (height_lock, beside z, a yes or
// no, unlocked for a .bms entity); a lock alone makes the .mis due, its save writing the locked entity's z as it stands
// with the terrain's height as its base and height_lock 1, an unlocked one's z less that height and no height_lock line;
// the lock read back from the .mis on the next open; a lock the file gives an SSN the mission lacks listed and left off;
// a lock the original editor's .mis does not hold staying unsaved, said in its words; and a building's, a marker's and
// an organic's lock read back as an item's (the .mis read with no items.def: every entity in its item pool).
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/file_source.h>

#include <editor/documents/mission_document.h>
#include <editor/documents/mission_names_file.h>
#include <editor/model/field_text.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_scene.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/mission/bms.h>
#include <formats/mission/bms_edit.h>
#include <formats/mission/mission_mis.h>
#include <formats/trn/trn_io.h>

#include "common/file_io.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

constexpr const char *kMission = "missions/locks.bms";
constexpr const char *kMis = "missions/locks.mis";

// Files by name, each put with a stamp of its own (the ground's files).
class MemoryFiles : public FileSource {
public:
	void put(const std::string &name, std::vector<uint8_t> bytes) { files_[strutil::to_lower(name)] = {std::move(bytes), ++serial_}; }
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files_.find(strutil::to_lower(name));
		if (found == files_.end()) return false;
		out = found->second.first;
		return true;
	}
	uint64_t stamp(const std::string &name) const override {
		const auto found = files_.find(strutil::to_lower(name));
		return found == files_.end() ? 0 : found->second.second;
	}

private:
	std::map<std::string, std::pair<std::vector<uint8_t>, uint64_t>> files_;
	uint64_t serial_ = 0;
};

// Each "  <key> <value>" line's value in the .mis text, in order.
std::vector<std::string> values_of(const std::string &text, const std::string &key) {
	std::vector<std::string> out;
	const std::string marker = "\n  " + key + " ";
	for (size_t at = 0; (at = text.find(marker, at)) != std::string::npos; ++at) {
		const size_t from = at + marker.size();
		const size_t end = text.find_first_of("\r\n", from);
		out.push_back(text.substr(from, end - from));
	}
	return out;
}

// A position line's z.
int64_t z_of(const std::string &position) { return std::stoll(position.substr(position.rfind(' ') + 1)); }

struct Project {
	test_temp::TempDir dir{"opennova_editor_mission_height_lock"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	std::string root;

	MissionDocument *mission() { return dynamic_cast<MissionDocument *>(session.document_for(kMission)); }
	std::vector<const Node *> items(MissionKind kind = MissionKind::Item) {
		return mission() ? mission()->rows_of(kind) : std::vector<const Node *>();
	}
	bool set_lock(size_t item, int64_t on, MissionKind kind = MissionKind::Item) {
		const std::vector<const Node *> rows = items(kind);
		if (item >= rows.size()) return false;
		Edit edit;
		edit.address = NodeAddress{rows[item]->id, rows[item]->kind, 0};
		edit.field = "height_lock";
		edit.value = on;
		session.handle(request::edit_record(kMission, edit));
		return session.outcome().done();
	}
	int64_t lock_of(size_t item, MissionKind kind = MissionKind::Item) {
		const std::vector<const Node *> rows = items(kind);
		Value value;
		if (item >= rows.size() || !mission()->get(NodeAddress{rows[item]->id, rows[item]->kind, 0}, "height_lock", value)) return -1;
		return std::get<int64_t>(value);
	}
	std::string mis() { return test_io::read_file_text(root + "/" + kMis); }
	void reopen() {
		session.handle(request::close_document(kMission));
		editor_test::handle_to_end(session, request::rescan());
		editor_test::handle_to_end(session, request::open_document(kMission));
	}
};

} // namespace

int main() {
	Project p;
	editor_test::handle_to_end(p.session, request::new_project(p.dir.file("project"), "Locks"));
	p.root = p.session.view().project.root;
	const std::vector<uint8_t> trn = test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.trn")),
	                           cpt = test_io::read_file(test_paths_fixture("terrain/tmap/Tmap.cpt"));
	TEST_EXPECT(!trn.empty() && !cpt.empty() && test_io::write_file(p.root + "/Tmap.trn", trn) &&
	            test_io::write_file(p.root + "/Tmap.cpt", cpt));
	// The terrain as the original editor samples it (mission::mis_terrain_height over the .trn's own grid).
	auto files = std::make_shared<MemoryFiles>();
	files->put("Tmap.trn", trn);
	files->put("Tmap.cpt", cpt);
	MissionGround ground;
	MissionSceneHeader header;
	header.terrain = "Tmap";
	ground.follow(files, 1, header, "missions/locks");
	TEST_EXPECT(ground.terrain());
	if (!ground.terrain()) return 1;
	TrnConfig config;
	std::string error;
	TEST_EXPECT(load_mission_trn(std::string(trn.begin(), trn.end()), TrnLaterTexts{}, config, error));
	mission::MisTerrain terrain;
	terrain.heights = ground.height_field()->heightmap;
	terrain.sectors = &config.sector_grid[0][0];
	terrain.origin_x = config.origin_x;
	terrain.origin_y = config.origin_y;
	terrain.wrap_x = config.wrap_x != 0;
	terrain.wrap_y = config.wrap_y != 0;
	// Two items on the island, each over ground of a height of its own.
	bms::File file;
	mission::BlankMission blank;
	blank.name = "Locks";
	blank.terrain = "Tmap";
	TEST_EXPECT(mission::make_blank(file, blank, error));
	std::vector<int32_t> under;
	for (int i = 0; i < 2; ++i) {
		bms::Entity entity = mission::new_entity(mission::EntityKind::Item, 106102, i + 1);
		entity.x = bms::to_fixed_16_16(-120.37 + 90.0 * i);
		entity.y = bms::to_fixed_16_16(40.61 - 70.0 * i);
		under.push_back(mission::mis_terrain_height(terrain, entity.x, entity.y));
		entity.z = under.back() + bms::to_fixed_16_16(2.0);
		TEST_EXPECT(entity.mis_height_lock == 0); // a new entity is unlocked, as the original editor's
		mission::entities(file, mission::EntityKind::Item)->push_back(entity);
	}
	TEST_EXPECT(under[0] != 0 && under[1] != 0 && under[0] != under[1]);
	mission::sync_counts(file);
	std::vector<uint8_t> bytes;
	TEST_EXPECT(bms::write(file, bytes, error) && test_io::write_file(p.root + "/" + kMission, bytes));
	editor_test::handle_to_end(p.session, request::rescan());
	p.session.handle(request::open_document(kMission));
	TEST_EXPECT(p.mission() != nullptr && p.items().size() == 2);
	if (!p.mission() || p.items().size() != 2) return 1;

	// The field: beside z, a yes or no, unlocked as read from the .bms.
	const std::vector<FieldSchema> &fields = p.mission()->fields(p.items()[0]->kind);
	size_t z_at = fields.size(), lock_at = fields.size();
	for (size_t i = 0; i < fields.size(); ++i) {
		if (fields[i].id == "z") z_at = i;
		if (fields[i].id == "height_lock") lock_at = i;
	}
	TEST_EXPECT(lock_at == z_at + 1 && lock_at < fields.size());
	if (lock_at >= fields.size()) return 1;
	TEST_EXPECT(is_yes_no(fields[lock_at]) && fields[lock_at].label == "Height lock" && fields[lock_at].group == "position" &&
	            fields[lock_at].description.find("follows the ground") != std::string::npos);
	TEST_EXPECT(p.lock_of(0) == 0 && p.lock_of(1) == 0);

	// No name and no lock: no .mis.
	TEST_EXPECT(!std::filesystem::exists(p.root + "/" + kMis) && !p.mission()->companion_unsaved());

	// A lock alone: unsaved until a save writes the .mis, the locked item absolute over its base, the other relative.
	TEST_EXPECT(p.set_lock(1, 1) && p.lock_of(1) == 1);
	TEST_EXPECT(p.mission()->companion_unsaved());
	p.session.handle(request::save(kMission));
	TEST_EXPECT(p.session.outcome().done() && !p.mission()->companion_unsaved());
	const std::string written = p.mis();
	const std::vector<std::string> positions = values_of(written, "position"), bases = values_of(written, "extra_bheight"),
	                               locks = values_of(written, "height_lock");
	TEST_EXPECT(positions.size() == 2 && bases.size() == 2 && locks.size() == 1 && locks[0] == "1");
	if (positions.size() != 2 || bases.size() != 2) return 1;
	const int32_t lifted = bms::to_fixed_16_16(2.0);
	TEST_EXPECT(z_of(positions[0]) == lifted && bases[0] == "0");
	TEST_EXPECT(z_of(positions[1]) == int64_t(under[1]) + lifted && bases[1] == std::to_string(under[1]));

	// Read back on the next open.
	p.reopen();
	TEST_EXPECT(p.mission() && p.lock_of(0) == 0 && p.lock_of(1) == 1 && !p.mission()->companion_unsaved());

	// A lock the file gives an SSN the mission lacks: listed, left off.
	{
		std::string text = written;
		const size_t id = text.find("\n  id 2");
		TEST_EXPECT(id != std::string::npos);
		if (id == std::string::npos) return 1;
		text.replace(id, 7, "\n  id 9");
		TEST_EXPECT(test_io::write_text(p.root + "/" + kMis, text));
		p.reopen();
		TEST_EXPECT(p.mission() && p.lock_of(1) == 0 && p.mission()->names_file().refused.size() == 1);
		bool listed = false;
		for (const Diagnostic &d : p.session.view().findings.diagnostics)
			listed = listed || (d.code() == "mission.names_refused" && d.asset == kMission &&
			                    d.message.find("SSN 9's height lock (the mission has no entity of that SSN") != std::string::npos);
		TEST_EXPECT(listed);
	}

	// The original editor's .mis (it writes what the editor's writer does not): its lock read, and a change of it
	// stays unsaved, said in its words.
	{
		const std::string original = "// mission metafile, saved on 10/11/2026, 12:00" + written.substr(written.find("\r\n"));
		TEST_EXPECT(test_io::write_text(p.root + "/" + kMis, original));
		p.reopen();
		TEST_EXPECT(p.mission() && p.mission()->names_file().found == MissionNamesFile::Found::Matched &&
		            !p.mission()->names_file().ours && p.lock_of(1) == 1);
		TEST_EXPECT(p.set_lock(1, 0));
		editor_test::handle_to_end(p.session, request::save(kMission));
		TEST_EXPECT(p.mis() == original && p.mission()->companion_unsaved());
		bool said = false;
		for (const Diagnostic &d : p.session.view().findings.diagnostics)
			said = said || (d.code() == "mission.names_foreign" && d.asset == kMission &&
			                d.message.find("The height locks the mission holds are not in it") != std::string::npos);
		TEST_EXPECT(said);
	}
	// Every pool's lock read back: an item, a building, a marker and an organic, the last three locked, saved, the
	// mission opened again. The .mis names no pool (each entity a `begin item`), so the lock goes to the mission's entity
	// of its SSN, whichever pool holds it.
	{
		// (The lock the original's .mis keeps unsaved: closed without it.)
		p.session.handle(request::close_document(kMission));
		if (p.session.view().dialogs.unsaved_prompt.open) p.session.handle(request::resolve_unsaved(UnsavedChoice::Discard));
		TEST_EXPECT(p.mission() == nullptr);
		std::filesystem::remove(p.root + "/" + kMis);
		bms::File pools;
		TEST_EXPECT(mission::make_blank(pools, blank, error));
		const std::pair<mission::EntityKind, int32_t> kinds[] = {{mission::EntityKind::Item, 106102},
		                                                         {mission::EntityKind::Building, 100701},
		                                                         {mission::EntityKind::Marker, 6001},
		                                                         {mission::EntityKind::Organic, 2001}};
		int32_t id = 1;
		for (const auto &[kind, ssn] : kinds) {
			bms::Entity entity = mission::new_entity(kind, ssn, id);
			entity.x = bms::to_fixed_16_16(-120.37 + 30.0 * id);
			entity.y = bms::to_fixed_16_16(40.61 - 20.0 * id);
			entity.z = mission::mis_terrain_height(terrain, entity.x, entity.y) + bms::to_fixed_16_16(2.0);
			mission::entities(pools, kind)->push_back(entity);
			++id;
		}
		mission::sync_counts(pools);
		std::vector<uint8_t> pool_bytes;
		TEST_EXPECT(bms::write(pools, pool_bytes, error) && test_io::write_file(p.root + "/" + kMission, pool_bytes));
		editor_test::handle_to_end(p.session, request::rescan());
		editor_test::handle_to_end(p.session, request::open_document(kMission));
		const MissionKind locked[] = {MissionKind::Building, MissionKind::Marker, MissionKind::Organic};
		for (const MissionKind kind : locked) {
			const size_t count = p.items(kind).size();
			const bool set = p.set_lock(0, 1, kind);
			if (count != 1 || !set)
				std::printf("  kind %d: %zu rows, set %d (items %zu, buildings %zu, markers %zu, organics %zu): %s\n", int(kind),
				            count, set ? 1 : 0, p.items(MissionKind::Item).size(), p.items(MissionKind::Building).size(),
				            p.items(MissionKind::Marker).size(), p.items(MissionKind::Organic).size(),
				            p.session.outcome().findings.empty() ? "" : p.session.outcome().findings[0].message.c_str());
			TEST_EXPECT(count == 1 && set);
		}
		editor_test::handle_to_end(p.session, request::save(kMission));
		TEST_EXPECT(values_of(p.mis(), "height_lock").size() == 3);
		p.reopen();
		TEST_EXPECT(p.mission() && p.mission()->names_file().found == MissionNamesFile::Found::Matched &&
		            p.mission()->names_file().refused.empty());
		TEST_EXPECT(p.lock_of(0) == 0);
		for (const MissionKind kind : locked) TEST_EXPECT(p.lock_of(0, kind) == 1);
	}
	std::printf("height lock: the field, a lock alone written, read back, an SSN the mission lacks, the original's, every "
	            "pool's\n");
	return 0;
}
