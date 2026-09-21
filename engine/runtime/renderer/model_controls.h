#pragma once

#include <runtime/renderer/material_eval.h>

#include <bitset>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova::renderer {

// One retained model's signed CTRL bus. Presence is distinct from zero; owner
// tags protect teardown, never stack overwritten values. The local part player
// writes this same bus through world::part_anim_step.
class ModelControls {
public:
	bool has(int ordinal) const;
	int32_t value(int ordinal) const;
	bool store(int ordinal, int64_t value, const std::string &owner = {});
	bool clear(int ordinal);
	bool clear_owned(int ordinal, const std::string &owner);
	std::vector<int> owned_registers(const std::string &owner) const;
	const std::vector<int> &present_registers() const { return present_order_; }
	ControlRegisterValues runtime_values(int32_t weather_flicker, int32_t weather_swing) const;
	// A position-hashed weather sample overwrites values without changing the
	// current writer's lifecycle tag, exactly like the old direct store.
	void sample_weather(int32_t flicker, int32_t swing);

	static int part_register(int channel);
	static const char *part_owner(int channel);
	void play_part(int channel, int direction, double seconds);
	enum class Restart { Invalid, Stopped, Seeded };
	Restart restart_part(int channel, int direction, double seconds);
	void release_part(int channel);
	void clear_parts();
	bool has_active_parts() const { return !parts_.empty(); }
	std::vector<int> active_part_registers() const;
	// True for an inserted register or a phase change, not merely for stopping
	// at an already-reached endpoint or releasing a writer's ownership tag.
	bool advance_parts(double delta);

private:
	static bool valid(int ordinal);
	void write_value(int ordinal, int32_t value);
	void erase_owner(int ordinal);
	ControlRegisterValues values_{};
	std::bitset<threedi::THREEDI_CTRL_REGISTER_COUNT> present_;
	std::vector<int> present_order_;
	std::unordered_map<int, std::string> owners_;
	std::vector<int> owner_order_;
	struct Part {
		int ordinal = 0;
		int32_t direction = 0;
		int32_t rate = 0;
		int32_t value = 0;
	};
	std::vector<Part> parts_;
	double part_tick_credit_ = 0.0;
};

} // namespace opennova::renderer
