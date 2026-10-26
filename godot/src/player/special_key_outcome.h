#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <runtime/inmatch/special_key_chain.h>

namespace godot {

// One key-down's walk through the special-key chain (engine inmatch::special_key_chain
// carries the chain and its witness) as the shell reads it: whether the chain took the key
// (it then reaches no binding row), and the device half the shell runs: the quit dialog's
// Exit Mission, the restart's splash re-run, the input event 12 to queue in key order with
// the rows' events, and the page a PgUp / PgDn steps. The digit arm's pick has already run.
class SpecialKeyOutcome : public RefCounted {
	GDCLASS(SpecialKeyOutcome, RefCounted)

	opennova::inmatch::SpecialKeyOutcome outcome_;

protected:
	static void _bind_methods();

public:
	enum Page {
		PAGE_NONE = static_cast<int>(opennova::inmatch::SpecialKeyPage::None),
		PAGE_SCOREBOARD = static_cast<int>(opennova::inmatch::SpecialKeyPage::Scoreboard),
		PAGE_HELP = static_cast<int>(opennova::inmatch::SpecialKeyPage::Help),
		PAGE_BRIEFING = static_cast<int>(opennova::inmatch::SpecialKeyPage::Briefing),
	};

	static Ref<SpecialKeyOutcome> make(const opennova::inmatch::SpecialKeyOutcome &p_outcome);

	bool is_taken() const { return outcome_.taken; }
	bool is_quit_confirmed() const { return outcome_.quit_confirmed; }
	bool is_restart_splash() const { return outcome_.restart_splash; }
	// The event-12 pick's parameter, -1 for none.
	int get_pick() const { return outcome_.pick; }
	int get_page() const { return static_cast<int>(outcome_.page); }
	bool is_page_forward() const { return outcome_.page_forward; }
};

} // namespace godot

VARIANT_ENUM_CAST(godot::SpecialKeyOutcome::Page);
