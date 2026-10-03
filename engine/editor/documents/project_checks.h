#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <vector>

#include <editor/assets/asset_kinds.h>
#include <editor/documents/project_check.h>
#include <editor/model/diagnostic.h>

namespace opennova::editor {

// The document types' project checks (ADR 0046 S13 V9; documents/project_check.h), one instance
// per type whose registry row makes one (DocumentType::project_check), keyed by its
// DocumentTypeId: the rows are constexpr, so what a check keeps from one validation to the next
// lives here, with whoever validates (the session's ProblemsService; the command line for its one
// validation). Each is made when this is, from the row the registry answers then, and made again
// at an update when the registry answers another row for its type (a test's stand-in put in place
// or gone). The validation cache follows such a swap only for the files it validates again (it
// keeps the others' findings until their stamps move), so a check made afresh after one reads
// records_checked answers the previous row made. The composition runs the checks after each
// file's own findings and puts their findings after the build's gate
// (project/project_findings.h).
class ProjectChecks {
public:
	ProjectChecks();
	~ProjectChecks();
	ProjectChecks(const ProjectChecks &) = delete;
	ProjectChecks &operator=(const ProjectChecks &) = delete;

	// Every check brought to the project as `input` has it, each once, in the registry's order:
	// true when one says its findings moved, or a check with findings went with its row.
	bool update(const ProjectCheckInput &input);
	// update a slot at a time, each a step at a time (S13 A3: a validation stepped over several
	// polls): the slots in the registry's order, `slot` from 0 to slot_count(). begin_slot starts the
	// slot's update (the first following the registry as update does); step_slot goes on within
	// `budget` bytes, true once the slot is up, `moved` then saying what update says for it. A slot
	// with no check is up in its first step.
	size_t slot_count() const { return slots_.size(); }
	bool has_check(size_t slot) const { return slot < slots_.size() && slots_[slot].check != nullptr; }
	void begin_slot(size_t slot);
	bool step_slot(size_t slot, const ProjectCheckInput &input, uint64_t budget, bool &moved);
	// Every check's findings as the last update left them, in the registry's order.
	void append_findings(std::vector<Diagnostic> &out) const;
	// What every check finds of one document alone (ProjectCheck::findings_of), in the registry's order.
	void findings_of(const DocumentBase &document, const ProjectCheckInput &input, std::vector<Diagnostic> &out) const;
	size_t findings_size() const;
	// The check of a type, null for a type with none. The type's own code reads what its check
	// keeps (the menu preview a screen's render: preview/menu_render_check.h's menu_render_check).
	const ProjectCheck *of(DocumentTypeId type) const;
	// The project closed: each check lets go of what it held of it (the checks stay).
	void clear();

private:
	struct Slot {
		std::unique_ptr<ProjectCheck> (*make)() = nullptr; // the row's hook the check was made by
		std::unique_ptr<ProjectCheck> check;
	};
	// Each type's check made again where the registry's row for the type has another hook; true
	// when a check with findings went.
	bool follow_registry();

	std::array<Slot, kDocumentTypeCount> slots_; // by DocumentTypeId, None left out
	bool registry_moved_ = false; // slot 0's begin made a check with findings go (follow_registry)
};

} // namespace opennova::editor
