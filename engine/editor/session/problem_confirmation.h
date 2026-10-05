#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/session/editor_request.h>
#include <editor/session/view/workspace_view.h>

namespace opennova::editor {

class ProblemFixCache;
class ProblemQueryCache;
class SessionCore;
struct ProblemFix;
struct ProblemQuery;
struct SessionView;

// What Problems asks before it acts, the session's (ADR 0046, the MCP gaps lane): a Fix all over a group's
// findings or the summary's over the required files', and a Use fix, wait in a confirmation for Apply. What a
// confirmation says and what its Apply raises are proposed here, from the findings as they are, for the window
// that draws it (ui/problems_list) and the wire alike: the workspace's confirmation (WorkspaceView::Problems::
// Confirm) proposed (propose_confirmation; the workspace section shows it) and applied (apply_confirmation).

// The query the workspace's Problems filters make: what the window shows, and the groups a confirmation's
// group key names.
ProblemQuery problem_query_of(const WorkspaceView::Problems &problems);

// A finding as it is known whatever its place among the findings (a validation between an ask and its Apply
// can move it): its code, its file, the record (its address and name), the field, the line and what it names
// (the file or the symbol, the role), then its ordinal among the findings before it alike in all of it.
std::string problem_finding_identity(const Diagnostic &diagnostic);
// The key of the finding at `index` of the view's findings ("" past the last), and the finding a key names
// now (SIZE_MAX: gone).
std::string problem_finding_key(const SessionView &view, size_t index);
size_t problem_finding_at(const SessionView &view, const std::string &key);

// Whether a fix waits in a confirmation before it acts: a Use fix renames a file and rewrites what names it,
// which Undo cannot take back. Any other fix is raised as it is, its detail in its tooltip.
bool fix_asks_first(const ProblemFix &fix);
// The fixes a field's reference picker offers of its value's finding: those raised at once alone (one that asks
// first waits in Problems' confirmation, which the picker has not; X16).
std::vector<ProblemFix> fixes_raised_at_once(std::vector<ProblemFix> fixes);

// What a confirmation says and what its Apply raises, and how many findings gave a fix.
struct ConfirmationProposal {
	std::vector<std::string> lines;
	std::vector<EditorRequest> requests;
	size_t findings = 0;
	bool operator==(const ConfirmationProposal &other) const {
		return lines == other.lines && requests == other.requests && findings == other.findings;
	}
	bool operator!=(const ConfirmationProposal &other) const { return !(*this == other); }
};
// A Fix all over `findings` (indices into the view's findings, a gone one passed over): each one's first bulk
// fix, merged (merge_fixes), only those of `only` when it is given (the summary's buttons), with what each
// request does in words, the placeholder textures in one line, and the line that says Undo cannot take it back.
ConfirmationProposal propose_fix_all(const SessionView &view, ProblemFixCache &fixes, const std::vector<size_t> &findings,
                                     const EditorRequestKind *only = nullptr);
// A finding's fix of `label`: its label and its detail (a Use fix's, what the rename rewrites today) and its
// request; nothing when the finding has no such fix now.
ConfirmationProposal propose_fix(const SessionView &view, ProblemFixCache &fixes, size_t finding, const std::string &label);
// The required files' findings (requirement.missing), what the summary's Fix alls act on.
std::vector<size_t> required_findings(const SessionView &view);

// The workspace's confirmation proposed over the view as it is: a group's Fix all (the group of that key in the
// answer the workspace's filters make, under a header, not the game's own data's; offered while two of its
// findings or more give a fix), the summary's of a request kind (offered while the required files' fixes make
// one), or a finding's fix of a label (the finding found by its key, else by its index). False with `why` when it
// names nothing the findings offer now: a refusal of set_workspace, of apply_confirmation.
bool propose_confirmation(const SessionView &view, const WorkspaceView::Problems::Confirm &confirm,
                          const WorkspaceView::Problems &problems, ProblemQueryCache &answers, ProblemFixCache &fixes,
                          ConfirmationProposal &out, std::string &why);

// What a request of a Fix all does, in its confirmation's words; a Fix all's button in the summary.
std::string fix_request_words(const SessionView &view, const EditorRequest &request);
std::string fix_all_label(const EditorRequest &request);

// ApplyConfirmation: the workspace's confirmation's Apply, as the window's raises it: its proposal now, each
// request served in order, then the confirmation closed. Refused (workspace.refused), nothing raised, while
// none is open or it proposes nothing.
void apply_confirmation(SessionCore &core);

} // namespace opennova::editor
