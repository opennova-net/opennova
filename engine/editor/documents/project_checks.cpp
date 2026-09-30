#include "project_checks.h"

#include <cassert>

#include <editor/documents/document_types.h>

namespace opennova::editor {

ProjectChecks::ProjectChecks() { follow_registry(); }

ProjectChecks::~ProjectChecks() = default;

bool ProjectChecks::follow_registry() {
	bool moved = false;
	for (size_t i = 0; i < slots_.size(); ++i) {
		const DocumentType *type = document_type(static_cast<DocumentTypeId>(i + 1));
		std::unique_ptr<ProjectCheck> (*make)() = type ? type->project_check : nullptr;
		Slot &slot = slots_[i];
		if (slot.make == make)
			continue;
		moved = moved || (slot.check && !slot.check->findings().empty());
		slot.make = make;
		slot.check = make ? make() : nullptr;
	}
#ifndef NDEBUG
	// A check is one type's, a stand-in's rows included, which document_types.cpp's static_assert
	// over the registered rows cannot see.
	for (size_t i = 0; i < slots_.size(); ++i)
		for (size_t j = i + 1; j < slots_.size(); ++j)
			assert((!slots_[i].make || slots_[i].make != slots_[j].make) &&
					"no two types' rows name one project check");
#endif
	return moved;
}

bool ProjectChecks::update(const ProjectCheckInput &input) {
	bool moved = follow_registry();
	for (Slot &slot : slots_)
		if (slot.check && slot.check->update(input))
			moved = true;
	return moved;
}

void ProjectChecks::append_findings(std::vector<Diagnostic> &out) const {
	for (const Slot &slot : slots_)
		if (slot.check)
			out.insert(out.end(), slot.check->findings().begin(), slot.check->findings().end());
}

size_t ProjectChecks::findings_size() const {
	size_t size = 0;
	for (const Slot &slot : slots_)
		size += slot.check ? slot.check->findings().size() : 0;
	return size;
}

const ProjectCheck *ProjectChecks::of(DocumentTypeId type) const {
	const size_t index = static_cast<size_t>(type);
	return index >= 1 && index <= slots_.size() ? slots_[index - 1].check.get() : nullptr;
}

void ProjectChecks::clear() {
	for (Slot &slot : slots_)
		if (slot.check)
			slot.check->clear();
}

} // namespace opennova::editor
