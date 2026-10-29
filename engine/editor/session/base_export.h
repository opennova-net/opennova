#pragma once

#include <memory>
#include <string>
#include <vector>

#include <editor/graph/graph_layer.h>
#include <editor/project/project_document.h>
#include <editor/run/null_process_platform.h>
#include <editor/session/base_layer_build.h>
#include <editor/session/build_operation.h>
#include <editor/session/preferences_store.h>
#include <editor/session/session_operation.h>
#include <editor/novaworld/novaworld_account.h>
#include <editor/novaworld/novaworld_transport.h>

namespace opennova::editor {

class ProjectSession;

// The Build, Play or Export a base export starts once the base is in (SessionCore::start_build's: the one
// that found the base's export missing or older than its project, and those that joined the export while it
// ran).
struct BuildAfterBase {
	bool wanted = false;
	PlayIntent play;
	ExportIntent exported;
	std::string out_dir;
	bool rehash = false;
	bool panel = true;
	std::string gate_tag_file;
	std::string version_file; // the expansion's own, never the base's export's
};

// An expansion of a project's base game (ADR 0046 T5) plays over that project's export, so the editor
// makes the export itself where it is missing or older than the project (project/base_project.h's
// base_export_of), as the server's release build exports a base game before its expansion
// (apps/novaworld_common/products/release_build.cpp) and as `opennova-project export` does: the base
// project opened in a session of its own (no process seam, the editor's preferences in memory, its
// workspace neither read nor written), its validation stepped as the editor's polls step it, then its
// Export (the build over its gate, the copy into its export folder, the stamp of its files recorded:
// export_build.h's `sources`), each step of that session one step of this operation within the poll's
// budget, so the editor keeps drawing. Then the base game read again from the export (the export before
// it where this one failed): its names (the Import fixes', the gate's) and its layer under the graph,
// stepped by bytes. finish() hands it to the session (SessionCore::absorb_base_export), which starts the
// Build, Play or Export that waited (BuildAfterBase) under this operation's id (hands_on_id). Cancelled,
// the base's session goes with its build and its copy, the export folder as it was.
class BaseExportOperation : public SessionOperation {
public:
	// `base_root`, the base game's project; `export_dir`, its export folder, the base game the expansion
	// reads; `expansion`, the open project's document (the base read again as it reads it); `preferences`,
	// the editor's (the game install a base project that names none opens with).
	BaseExportOperation(std::string base_root, std::string export_dir, ProjectDocument expansion,
			Preferences preferences, BuildAfterBase after, NovaWorldAccount account = {},
			NovaWorldTransport *transport = nullptr);
	~BaseExportOperation() override;

	OperationKind kind() const override { return OperationKind::BaseExport; }
	bool step(const StepBudget &budget) override;
	OperationProgress progress() const override;
	void cancel() override;
	// A Build, a Play or an Export starts once the base is in.
	void join(const EditorRequest &request) override;
	bool hands_on_id() const override { return ok_ && after_.wanted; }
	OperationOutcome finish(SessionCore &core) override;

	bool ok() const { return ok_; }
	const std::string &base_root() const { return root_; }
	const std::string &export_dir() const { return export_dir_; }
	// Why the base project was not exported, in its first refusal's words ("" when it was).
	const std::string &reason() const { return reason_; }
	const BuildAfterBase &after() const { return after_; }
	// The base game as the expansion reads it now: the names the Import fixes read, those its gate reads,
	// and its layer under the graph (null where the folder holds no base game).
	std::vector<std::string> &install_files() { return install_files_; }
	std::vector<std::string> &base_files() { return base_files_; }
	std::shared_ptr<const GraphLayer> take_base_layer() { return layer_ ? layer_->take() : nullptr; }

private:
	enum class Phase { Open, Settle, Export, Read, Done };

	// One poll of the base project's session within `budget`: true while what the export waits on runs (its
	// operation, and with `validation` its validation).
	bool base_busy(const StepBudget &budget, bool validation);
	// The base project not exported, `why`: the base game read as it stands.
	bool fail(std::string why);

	std::string root_;
	std::string export_dir_;
	ProjectDocument expansion_;
	NullProcessPlatform platform_;
	MemoryPreferencesStore preferences_;
	MemoryNovaWorldAccountStore accounts_;
	NovaWorldTransport *transport_ = nullptr;
	std::unique_ptr<ProjectSession> session_; // the base project's, while it opens and exports
	BuildAfterBase after_;
	Phase phase_ = Phase::Open;
	bool ok_ = false;
	std::string reason_;
	bool listed_ = false; // the base game's names read again
	std::vector<std::string> install_files_;
	std::vector<std::string> base_files_;
	std::unique_ptr<BaseLayerBuild> layer_;
};

} // namespace opennova::editor
