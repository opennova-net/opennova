#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/documents/project_check.h>
#include <editor/model/diagnostic.h>
#include <editor/preview/mission_ground_facts.h>
#include <editor/preview/mission_ground_rules.h>
#include <editor/preview/viewport_follow.h>

namespace opennova::editor {

class MissionDocument;
class ProjectChecks;

// What a step of the ground check spends on a mission it does not check again (S13 A3); a mission it
// checks ends the step.
inline constexpr uint64_t kMissionGroundStepCost = 4096;

// The ground check (the deep-integration plan's DI-28), the mission type's project check (S13 V9: its
// registry row makes it, make_mission_ground_check.h): every mission of the project, its entities
// grounded by the game's own rules (mission_ground_rules.h) over the project's files as the game looks
// them up, and each one the game leaves off the ground a mission.off_ground finding on its record's z,
// a Warning whose planned fix (Diagnostic::planned, FindingFix::EditRecord) sets the z where the rule
// stands it. A mission is checked again only when its document state moved (an open one's revision, a
// closed one's scan row) or a file its check read moved its stamp (the item table, a model, the terrain,
// a person's table or clip); a mission a step. Its findings are never part of the build's gate.
class MissionGroundCheck : public ProjectCheck {
public:
	bool update(const ProjectCheckInput &input) override;
	void begin() override;
	bool step(const ProjectCheckInput &input, uint64_t budget, bool &moved) override;
	void clear() override;
	const std::vector<Diagnostic> &findings() const override { return diagnostics_; }
	// The mission at `path` as the last update grounded it, in its scene's order (null: none checked); one
	// entity's verdict (null: none).
	const std::vector<MissionGroundVerdict> *verdicts(const std::string &path) const;
	const MissionGroundVerdict *verdict(const std::string &path, NodeId row) const;
	// Missions checked again by the last update (not reused), and the files its reads parsed in all (tests).
	size_t checked() const { return checked_; }
	size_t parsed() const { return reads_.parsed(); }

private:
	struct Mission {
		bool seen = false;
		bool made = false;
		// What it was made from: an open document's state, or a closed file's scan row.
		bool open = false;
		uint64_t identity = 0, load_generation = 0, revision = 0;
		uint64_t size = 0;
		int64_t modified = 0;
		AssetKind kind = AssetKind::Unknown;
		std::string game;
		FileStamps stamps; // every file its verdicts read
		std::vector<MissionGroundVerdict> verdicts;
		std::vector<Diagnostic> findings;
	};
	void check_(Mission &mission, const MissionDocument *document, const FileSource &files);

	std::map<std::string, Mission> missions_; // by project-relative path
	MissionGroundReads reads_;
	std::unique_ptr<MissionGround> ground_ = std::make_unique<MissionGround>(); // the last mission checked's ground
	uint64_t generation_ = 0;
	std::vector<Diagnostic> diagnostics_;
	size_t checked_ = 0;
	struct Cursor {
		bool started = false;
		size_t next = 0;
	};
	Cursor cursor_;
	bool moved_ = false;
};

// The ground check among a validation's project checks (the mission type's): null when `checks` is null
// or the mission type's check is not the ground check (a test's stand-in in its place).
const MissionGroundCheck *mission_ground_check(const ProjectChecks *checks);

} // namespace opennova::editor
