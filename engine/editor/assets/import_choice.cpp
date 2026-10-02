#include <editor/assets/import_choice.h>

#include <editor/project/project_files.h>

namespace opennova::editor {

std::string ImportChoice::name() const {
	return entry.empty() ? basename_of(path) : entry;
}

} // namespace opennova::editor
