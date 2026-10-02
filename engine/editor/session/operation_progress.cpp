#include <editor/session/operation_progress.h>

namespace opennova::editor {

const char *operation_unit_token(OperationUnit unit) {
	switch (unit) {
	case OperationUnit::Bytes: return "bytes";
	case OperationUnit::Files: return "files";
	case OperationUnit::Steps: return "steps";
	}
	return "steps";
}

} // namespace opennova::editor
