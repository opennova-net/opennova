#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <terrain/builder.h>

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace godot {

class NovaTerrainBuilder;

class NovaTerrainBuildJob : public RefCounted {
	GDCLASS(NovaTerrainBuildJob, RefCounted)

private:
	struct State {
		bool running = false;
		bool finished = false;
		float ratio = 0.0f;
		int current = 0;
		int total = 0;
		Error result_error = OK;
		std::string phase;
		std::string message;
	};

	mutable std::mutex _mutex;
	std::thread _thread;
	State _state;

	void _join_thread();
	void _set_progress(const std::string &phase,
	                   const std::string &message,
	                   int current,
	                   int total,
	                   float ratio);
	void _set_result(Error error, const std::string &message);
	void _start_data(std::vector<uint8_t> heightmap_raw16,
	                 std::string output_dir,
	                 std::string terrain_name,
	                 std::string creator,
	                 opennova::DepthFormat depth_format,
	                 opennova::TerrainQuadrantLocks quadrant_locks);

protected:
	static void _bind_methods();

public:
	NovaTerrainBuildJob();
	~NovaTerrainBuildJob();

	bool is_running() const;
	bool is_finished() const;
	float get_progress_ratio() const;
	int get_progress_current() const;
	int get_progress_total() const;
	String get_progress_message() const;
	String get_progress_phase() const;
	Error get_result_error() const;
	String get_result_message() const;
	void wait_for_completion();

	friend class NovaTerrainBuilder;
};

} // namespace godot
