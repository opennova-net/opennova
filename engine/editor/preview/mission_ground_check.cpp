// The mission type's project check: what the game grounds where (mission_ground_check.h, DI-28).

#include <editor/preview/mission_ground_check.h>

#include <algorithm>

#include <editor/documents/document_types.h>
#include <editor/documents/mission_document.h>
#include <editor/documents/mission_validation.h>
#include <editor/documents/project_checks.h>
#include <editor/preview/make_mission_ground_check.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/mission_source.h>
#include <editor/project/project_document.h>

namespace opennova::editor {

namespace {

// The mission's own name: the game reads <mission>.til beside it (MissionGround).
std::string mission_name(const std::string &path) {
	std::string name = path;
	if (const size_t slash = name.find_last_of("/\\"); slash != std::string::npos) name.erase(0, slash + 1);
	if (const size_t dot = name.find_last_of('.'); dot != std::string::npos) name.erase(dot);
	return name;
}

std::shared_ptr<const MissionDocument> read_mission(const ValidationInput &input, const AssetEntry &asset) {
	auto document = std::make_shared<MissionDocument>();
	Diagnostic error;
	if (!load_listed(*document, input.paths, asset, input.project.target_game, error) || document->blocked())
		return nullptr;
	return document;
}

} // namespace

void MissionGroundCheck::clear() {
	missions_.clear();
	reads_.clear();
	ground_ = std::make_unique<MissionGround>();
	diagnostics_.clear();
	checked_ = 0;
	cursor_ = Cursor();
	moved_ = false;
}

bool MissionGroundCheck::update(const ProjectCheckInput &input) {
	begin();
	bool moved = false;
	while (!step(input, UINT64_MAX, moved)) {
	}
	return moved;
}

void MissionGroundCheck::begin() { cursor_ = Cursor(); }

void MissionGroundCheck::check_(Mission &mission, const MissionDocument *document, const FileSource &files) {
	mission.made = true;
	mission.stamps.clear();
	mission.verdicts.clear();
	moved_ = moved_ || !mission.findings.empty();
	mission.findings.clear();
	if (!document) return;
	const std::unique_ptr<MissionSceneSource> source = mission_scene_source(*document);
	if (!source) return;
	MissionScene scene;
	scene.read(*source);
	// Every file the rules read noted with its stamp, the terrain's and the people's clips included.
	const std::shared_ptr<const FileSource> served(std::shared_ptr<const FileSource>(), &files);
	const auto stamped = std::make_shared<StampedFiles>(served);
	ground_->follow(stamped, ++generation_, scene.header(), mission_name(document->path()));
	mission.verdicts = mission_ground_verdicts(scene, *ground_, reads_, stamped);
	mission.stamps = stamped->stamps();
	for (const MissionGroundVerdict &verdict : mission.verdicts) {
		if (!verdict.off()) continue;
		const NodeAddress address{ verdict.row, verdict.kind, 0 };
		const MissionEntityMark *entity = scene.entity(verdict.row);
		// The entity in a modder's words: its item's name and its SSN ("Wooden crate #12").
		const std::string title = !verdict.name.empty() && entity
				? verdict.name + " #" + std::to_string(entity->ssn)
				: document->record_title(address);
		Diagnostic d = make_finding(MissionFinding::OffGround, DiagnosticSeverity::Warning,
				mission_ground_message(verdict, title), document->path(), "z");
		d.record = document->record_name(address);
		d.record_title = title;
		d.record_key = document->record_identity(address);
		d.row_id = verdict.row;
		d.record_kind = verdict.kind;
		// Its fix (FindingFix::EditRecord): the z where the rule stands it, an edit of the mission.
		Edit set;
		set.address = address;
		set.field = "z";
		set.value = verdict.fix_z;
		PlannedFix fix;
		mission_ground_fix_words(verdict, fix.label, fix.detail);
		fix.edits.push_back(std::move(set));
		d.planned.push_back(std::move(fix));
		mission.findings.push_back(std::move(d));
	}
	moved_ = moved_ || !mission.findings.empty();
}

bool MissionGroundCheck::step(const ProjectCheckInput &input, uint64_t budget, bool &moved) {
	const ValidationInput &validation = input.validation;
	if (!cursor_.started) {
		cursor_.started = true;
		checked_ = 0;
		for (auto &entry : missions_) entry.second.seen = false;
	}
	uint64_t spent = 0;
	const std::vector<AssetEntry> &entries = validation.scan.entries;
	while (cursor_.next < entries.size()) {
		if (spent >= budget) return false;
		const AssetEntry &asset = entries[cursor_.next++];
		if (asset.kind != AssetKind::Mission) continue;
		spent += kMissionGroundStepCost;
		Mission &kept = missions_[asset.relative_path];
		kept.seen = true;
		const auto open = std::dynamic_pointer_cast<const MissionDocument>(validation.open_document(asset));
		bool stale = !kept.made || kept.stamps.moved(input.files);
		if (open) {
			stale = stale || !kept.open || kept.identity != open->identity() ||
					kept.load_generation != open->load_generation() || kept.revision != open->revision();
		} else {
			stale = stale || kept.open || kept.size != asset.size_bytes || kept.modified != asset.modified_ticks ||
					kept.kind != asset.kind || kept.game != validation.project.target_game;
		}
		if (!stale) continue;
		kept.open = open != nullptr;
		if (open) {
			kept.identity = open->identity();
			kept.load_generation = open->load_generation();
			kept.revision = open->revision();
		}
		kept.size = asset.size_bytes;
		kept.modified = asset.modified_ticks;
		kept.kind = asset.kind;
		kept.game = validation.project.target_game;
		// A closed mission read for its check alone where its own checks read its records (one that does not
		// load, or that a source error blocks, has its own findings), and let go after.
		std::shared_ptr<const MissionDocument> closed;
		if (!open && input.cache.records_checked(asset.relative_path)) closed = read_mission(validation, asset);
		const MissionDocument *document = open ? open.get() : closed.get();
		check_(kept, document && !document->blocked() ? document : nullptr, input.files);
		++checked_;
		return false; // a mission a step
	}
	for (auto it = missions_.begin(); it != missions_.end();) {
		if (it->second.seen) {
			++it;
			continue;
		}
		moved_ = moved_ || !it->second.findings.empty();
		it = missions_.erase(it);
	}
	moved = moved_;
	moved_ = false;
	cursor_ = Cursor();
	if (!moved) return true;
	diagnostics_.clear();
	for (const auto &entry : missions_)
		diagnostics_.insert(diagnostics_.end(), entry.second.findings.begin(), entry.second.findings.end());
	return true;
}

const std::vector<MissionGroundVerdict> *MissionGroundCheck::verdicts(const std::string &path) const {
	const auto found = missions_.find(path);
	return found == missions_.end() || !found->second.made ? nullptr : &found->second.verdicts;
}

const MissionGroundVerdict *MissionGroundCheck::verdict(const std::string &path, NodeId row) const {
	const std::vector<MissionGroundVerdict> *all = verdicts(path);
	if (!all) return nullptr;
	for (const MissionGroundVerdict &verdict : *all)
		if (verdict.row == row) return &verdict;
	return nullptr;
}

std::unique_ptr<ProjectCheck> make_mission_ground_check() { return std::make_unique<MissionGroundCheck>(); }

const MissionGroundCheck *mission_ground_check(const ProjectChecks *checks) {
	return checks ? dynamic_cast<const MissionGroundCheck *>(checks->of(DocumentTypeId::Mission)) : nullptr;
}

} // namespace opennova::editor
