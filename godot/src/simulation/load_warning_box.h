#pragma once

#include <runtime/mission/promote.h> // mission::LoadWarningAnswer / LoadWarningBox

#include <string>

// The mission load's warning box (engine mission::LoadWarningBox, KernelBootOptions::load_warning):
// a blocking OK/Cancel box over the game's window, the cursor shown while it is up. OK carries the
// load on; Cancel stops it, and the shell ends the process (Simulation::load_ended_process). A run
// with no window (headless) answers OK, the engine's log line its only trace.
opennova::mission::LoadWarningAnswer load_warning_box(
		const std::string &p_caption, const std::string &p_text);
