#include <editor/assets/import_source.h>

#include <editor/project/project_files.h>

namespace opennova::editor {

std::string ImportSource::name() const {
	return entry.empty() ? basename_of(path) : entry;
}

} // namespace opennova::editor
