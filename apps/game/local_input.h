#pragma once

#include <base/platform/input_device.h>
#include <runtime/controls/binding_set.h>
#include <runtime/inmatch/platform_input_device.h>
#include <runtime/inmatch/player_input_frame.h>
#include <runtime/inmatch/session.h>

// The local player's input for opennova-game: the platform's raw events into the engine's input
// frame (inmatch::PlayerInputFrame over inmatch::PlatformInputDevice and the catalog's default
// bindings), which the session's updates sample. The frame's seams onto the session and the shell
// are the narrow ones a mission with no menus and no HUD has: the key scan's rows and the
// pointer's look reach the session's ticks; the action requests past the dispatch head (the
// stance, the weapon rows, the seats), the chat line, the special keys and the shell's records
// (the Escape row's INGAME, the screen shot) are the engine frontend's (R3), and do nothing here.
namespace opennova::game {

class LocalMission;

class LocalInput {
public:
	explicit LocalInput(LocalMission &mission);
	LocalInput(const LocalInput &) = delete;
	LocalInput &operator=(const LocalInput &) = delete;

	// The platform's events since the last pump, in order, onto the device (every key into its
	// state and queue) and, under live gameplay with a player, the frame (the pointer's motion,
	// its buttons and the wheel); then the device's state snapshot.
	void pump(platform::InputDevice &input);
	// The frame's head (PlayerInputFrame::begin_frame) onto `frame`: the cursor offset its first
	// update reads and the input frame as every update's source.
	void begin_frame(inmatch::FrameInput &frame);

private:
	// The session the frame's records reach: the mission's.
	class Session final : public inmatch::PlayerInputSession {
	public:
		explicit Session(LocalMission &mission) : mission_(mission) {}
		bool available() const override;
		Facts facts() const override;
		bool action_head_admits(int code) override;
		controls::UseItemPressArm take_use_item_press() override;
		void run_action(const controls::PlayerActionRequest &request) override;
		bool dispatch_action_record(int32_t code, int32_t param) override;
		void left_button_down() override {}
		void clear_deferred_input() override {}
		void dispatch_talk_rows(uint32_t rows_fired, bool active) override;
		inmatch::SpecialKeyOutcome special_key_chain(hud::HudToggleState &toggles,
				controls::PlayerActions &actions, const inmatch::SpecialKeyInput &input) override;

	private:
		LocalMission &mission_;
	};

	// The shell: its keyboard pass over the device's key queue, no menu screen, no records.
	class Shell final : public inmatch::PlayerInputShell, private controls::KeyboardPassLegs {
	public:
		Shell(inmatch::PlatformInputDevice &device, inmatch::PlayerInputFrame &frame) :
				device_(device), frame_(frame) {}
		void keyboard_pass() override;
		bool menu_screen_open() override { return false; }
		void record_leg(int32_t code, int32_t param) override;

	private:
		// controls::KeyboardPassLegs: no text line and no special keys take a key.
		void release_pass() override;
		void dequeued(const controls::KeyEvent &event, int slot) override;
		bool chat_key(const controls::KeyEvent &event) override;
		bool special_key(const controls::KeyEvent &event) override;
		int binding_code(int scan_code) override;
		void row_event(const controls::KeyEvent &event, int code, int scan_code) override;

		inmatch::PlatformInputDevice &device_;
		inmatch::PlayerInputFrame &frame_;
		bool sign_ = false; // the last scan's fire sign, which its row event carries
	};

	LocalMission &mission_;
	controls::BindingSet bindings_; // the catalog's defaults
	inmatch::PlatformInputDevice device_;
	inmatch::PlayerInputFrame frame_;
	Session session_;
	Shell shell_;
};

} // namespace opennova::game
