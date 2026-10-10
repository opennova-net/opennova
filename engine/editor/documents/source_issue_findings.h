#pragma once

#include <functional>
#include <vector>

#include <editor/model/diagnostic.h>
#include <editor/model/document.h>

namespace opennova::editor {

// The findings a document's source issues make (Document::issues(): the input its reader left
// out, or its model cannot hold), in their order, each type's validator calling it with its own
// rows: a blocking issue an error under `invalid` (it blocks the save and the build), any other a
// warning under `ignored` (a save drops it), on the issue's field, line and record,
// and on the record its locator names in the file as loaded, wherever that record is now
// (Document::source_address; gone since, or no locator: the file alone). `place`, when given,
// then places each finding the type's own way (a catalog finds its record by name). `game_reads`, when
// given, ends each blocking finding under `invalid`: what the game does with the input, which it reads on
// (an unwritable_code's words). `stops`, when given, takes a blocking issue the game's reader stops at
// (SourceIssue::game_stops).
void source_issue_findings(const Document &document, const FindingCodeRow &invalid,
		const FindingCodeRow &ignored, std::vector<Diagnostic> &findings,
		const std::function<void(Diagnostic &)> &place = nullptr, const char *game_reads = nullptr,
		const FindingCodeRow *stops = nullptr);

} // namespace opennova::editor
