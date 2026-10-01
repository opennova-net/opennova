#pragma once

#include <cstdint>
#include <string>

namespace opennova::editor {

// What a progress counts (ADR 0046 S13 A1): the bytes an operation's steps read, hash, write or
// copy; the files; or steps of its own.
enum class OperationUnit : uint8_t { Bytes, Files, Steps };
// "bytes", "files", "steps".
const char *operation_unit_token(OperationUnit unit);

// Where an operation stands: `done` of `total` (never more, never going back), in `unit`, and
// what it works on now ("Packing localres.pff"). A viewport's device reports its build's units in
// one too (S13 V6, preview/viewport_build_report.h): its own for each build generation, so a newer
// generation's begins again at 0. A leaf, so what reports a progress takes no edge into the
// session (session_operation.h, which holds the operations themselves, includes it).
struct OperationProgress {
	uint64_t done = 0;
	uint64_t total = 0;
	OperationUnit unit = OperationUnit::Steps;
	std::string label;
};

} // namespace opennova::editor
