#pragma once

#include <functional>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document.h>
#include <editor/session/finding_codes.h>

namespace opennova::editor {

// The findings a document's source issues make (Document::issues(): the input its reader left
// out, or its model cannot hold), in their order, each type's validator calling it with its own
// rows: a blocking issue an error under `invalid` (it blocks the save and the build), any other a
// warning under `ignored` (a save drops it), on the issue's field, line and record,
// and on the record its locator names in the file as loaded, wherever that record is now
// (Document::source_address; gone since, or no locator: the file alone). `place`, when given,
// then places each finding the type's own way (a catalog finds its record by name).
void source_issue_findings(const Document &document, const FindingCodeRow &invalid,
		const FindingCodeRow &ignored, std::vector<Diagnostic> &findings,
		const std::function<void(Diagnostic &)> &place = nullptr);

} // namespace opennova::editor
