#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/model/diagnostic.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/problem_query.h>

namespace opennova::editor {

class MenuRenderCheck;
class SessionCore;
struct SessionView;

// The Problems rows of the project session (ADR 0046 S13 A2): when the project validates (an edit
// leaves it due rather than running it; a request from outside returns validated, and a pump that
// holds validation validates once, at its poll) and what the one findings composer reads
// (compose_project_findings, project/project_findings: the asset graph, the validation cache, the
// menu render check and the project's files as the open documents stand in for theirs; the last
// Play's own findings; the last build's own; the open documents' own). The findings a request
// reports are an input of their own (add_reported), shown after the composed rows, so reporting a
// finding never validates: the validation an edit had left due when it was reported keeps it (it
// ran first, when report() validated), and a later one drops it. A kept finding is compared with
// the composed rows alone: one the composition makes too is shown once, and a finding reported
// twice is two rows. It keeps the Problems query and the fixes the editor MCP asks for
// (problems_json), each until what it reads moves.
class ProblemsService {
public:
	explicit ProblemsService(SessionCore &core);
	ProblemsService(const ProblemsService &) = delete;
	ProblemsService &operator=(const ProblemsService &) = delete;

	// The project's findings now, composed; they replace the Problems rows (Findings moves only
	// when they differ, and Graph only when the graph's update changed it).
	void validate_documents();
	// An edit's validation, left for validate_pending: the request's return from outside, a pump's
	// poll, or a flow that reads the graph. Nothing in the view moves until it runs.
	void validate_later() {
		validation_due_ = true;
		for (Reported &reported : reported_) reported.kept = false; // a change after them
	}
	void validate_pending() {
		if (validation_due_) validate_pending_now();
	}
	// A pump holds validation until its poll (hold), which releases it (release).
	void hold() { validation_held_ = true; }
	void release() { validation_held_ = false; }
	bool held() const { return validation_held_; }

	// A finding a request or a poll reported: a Problems row now, and after the composed rows until
	// the validation for a change made after it.
	void add_reported(const Diagnostic &d);

	// The project's files as the scan lists them, the open documents standing in (set_open).
	void set_scan(const std::string &root, const AssetScan &scan, const std::string &game) {
		assets_->set_scan(root, scan, game);
	}
	void set_open(const std::vector<std::shared_ptr<const DocumentBase>> &documents) {
		assets_->set_open(documents);
	}
	// The last build's own findings, Problems rows until the next build starts or the project closes.
	void set_build_findings(std::vector<Diagnostic> findings) { build_findings_ = std::move(findings); }
	void clear_build_findings() { build_findings_.clear(); }
	// The last Play's own (a nonzero exit), rows until Play starts again or the project closes.
	void set_play_findings(std::vector<Diagnostic> findings) { play_findings_ = std::move(findings); }
	// What the open project held goes (close_project): the graph emptied under a new generation, the
	// render check, the cache and the last validation's findings; nothing left due.
	void clear();

	const AssetGraph &graph() const { return *graph_; }
	// The last validation's document findings, which the build plan gates on (the render check's
	// notes are not: they never block a build).
	const std::vector<Diagnostic> &document_findings() const { return document_findings_; }
	// What the last validation read: the closed files it loaded and reused.
	const ValidationStats &validation_stats() const { return validation_cache_.stats(); }

	// The Problems query as the editor MCP asks it, as JSON text (session_json's
	// problem_query_from_json, problems_to_json): {total, shown, counts, groups when grouped,
	// problems with their fixes}, or {error} for a query that does not parse. The answer and the
	// fixes are kept while what they read stands. (S13 A5's query seam takes it as its `problems`
	// row.)
	std::string problems_json(const std::string &query);

private:
	// A reported finding, and whether the validation due when it was reported keeps it (none was
	// due, or a change came after it: the next validation drops it).
	struct Reported {
		Diagnostic finding;
		bool kept = false;
	};

	void validate_pending_now();
	void compose(bool keep_reported);

	SessionCore &core_;
	SessionView &view_;
	std::shared_ptr<AssetGraph> graph_ = std::make_shared<AssetGraph>();
	std::shared_ptr<ProjectAssetSource> assets_ = std::make_shared<ProjectAssetSource>();
	std::shared_ptr<MenuRenderCheck> render_check_;
	ValidationCache validation_cache_;
	std::vector<Diagnostic> document_findings_; // the last validation's, which the build plan gates on
	                                            // (the render check's notes are not: they never block a build)
	// The last build's own findings (those its report adds to the Problems rows it was gated on:
	// the plan's own, a step that failed), Problems rows until the next build starts or the
	// project closes.
	std::vector<Diagnostic> build_findings_;
	std::vector<Diagnostic> play_findings_; // the last Play's own (a nonzero exit), that project's too
	std::vector<Reported> reported_;        // reported since the last validation
	bool validation_due_ = false;           // an edit since the last validation
	bool validation_held_ = false;          // a pump holds validation until its poll
	ProblemQueryCache query_cache_;         // problems_json's answer while the view and query stand
	ProblemFixCache fix_cache_;             // and its problems' fixes while the view stands
};

} // namespace opennova::editor
