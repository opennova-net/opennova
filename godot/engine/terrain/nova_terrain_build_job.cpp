#include "nova_terrain_build_job.h"

#include "nova_terrain_builder.h"

#include <godot_cpp/core/class_db.hpp>

#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
#include <eh.h>
#include <windows.h>
// Translate Windows structured exceptions (access violations, divide-by-zero,
// stack overflow, __fastfail, …) into std::runtime_error so the C++ catch
// blocks below can report what happened instead of silently yielding
// "Unknown build failure." Requires the TU be compiled with /EHa.
static void _seh_to_std(unsigned code, EXCEPTION_POINTERS *info) {
	const void *addr = info ? info->ExceptionRecord->ExceptionAddress : nullptr;
	char buf[160];
	std::snprintf(buf, sizeof(buf),
	              "native exception 0x%08X at %p (SEH)", code, addr);
	throw std::runtime_error(buf);
}
#endif

namespace godot {

NovaTerrainBuildJob::NovaTerrainBuildJob() = default;

NovaTerrainBuildJob::~NovaTerrainBuildJob() {
	_join_thread();
}

void NovaTerrainBuildJob::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_running"), &NovaTerrainBuildJob::is_running);
	ClassDB::bind_method(D_METHOD("is_finished"), &NovaTerrainBuildJob::is_finished);
	ClassDB::bind_method(D_METHOD("get_progress_ratio"), &NovaTerrainBuildJob::get_progress_ratio);
	ClassDB::bind_method(D_METHOD("get_progress_current"), &NovaTerrainBuildJob::get_progress_current);
	ClassDB::bind_method(D_METHOD("get_progress_total"), &NovaTerrainBuildJob::get_progress_total);
	ClassDB::bind_method(D_METHOD("get_progress_message"), &NovaTerrainBuildJob::get_progress_message);
	ClassDB::bind_method(D_METHOD("get_progress_phase"), &NovaTerrainBuildJob::get_progress_phase);
	ClassDB::bind_method(D_METHOD("get_result_error"), &NovaTerrainBuildJob::get_result_error);
	ClassDB::bind_method(D_METHOD("get_result_message"), &NovaTerrainBuildJob::get_result_message);
	ClassDB::bind_method(D_METHOD("wait_for_completion"), &NovaTerrainBuildJob::wait_for_completion);
}

void NovaTerrainBuildJob::_join_thread() {
	if (_thread.joinable()) {
		_thread.join();
	}
}

void NovaTerrainBuildJob::_set_progress(const std::string &phase,
                                        const std::string &message,
                                        int current,
                                        int total,
                                        float ratio) {
	std::lock_guard<std::mutex> lock(_mutex);
	_state.phase = phase;
	_state.message = message;
	_state.current = current;
	_state.total = total;
	_state.ratio = ratio;
}

void NovaTerrainBuildJob::_set_result(Error error, const std::string &message) {
	std::lock_guard<std::mutex> lock(_mutex);
	_state.running = false;
	_state.finished = true;
	_state.result_error = error;
	_state.message = message;
	if (error == OK) {
		_state.ratio = 1.0f;
		if (_state.total > 0) {
			_state.current = _state.total;
		}
		_state.phase = "complete";
	}
}

void NovaTerrainBuildJob::_start_data(std::vector<uint8_t> heightmap_raw16,
                                      std::string output_dir,
                                      std::string terrain_name,
                                      std::string creator,
                                      opennova::DepthFormat depth_format,
                                      opennova::TerrainQuadrantLocks quadrant_locks) {
	_join_thread();
	{
		std::lock_guard<std::mutex> lock(_mutex);
		_state = State{};
		_state.running = true;
		_state.phase = "load_depthmap";
		_state.message = "Queued build job...";
	}

	_thread = std::thread([this,
	                       heightmap_raw16 = std::move(heightmap_raw16),
	                       output_dir = std::move(output_dir),
	                       terrain_name = std::move(terrain_name),
	                       creator = std::move(creator),
	                       depth_format,
	                       quadrant_locks]() mutable {
#ifdef _MSC_VER
		_set_se_translator(&_seh_to_std);
#endif
		try {
			auto result = NovaTerrainBuilder::_build_from_data_impl(
				heightmap_raw16,
				output_dir,
				terrain_name,
				creator,
				quadrant_locks,
				depth_format,
				[this](const opennova::TerrainBuildProgress &progress) {
					_set_progress(progress.phase, progress.message,
					              progress.current, progress.total, progress.ratio);
				});
			_set_result(result.error, result.message);
		} catch (const std::bad_alloc &e) {
			_set_result(ERR_OUT_OF_MEMORY,
			            std::string("Out of memory: ") + e.what());
		} catch (const std::exception &e) {
			_set_result(ERR_SCRIPT_FAILED, e.what());
		} catch (...) {
			_set_result(ERR_SCRIPT_FAILED, "Unknown build failure.");
		}
	});
}

bool NovaTerrainBuildJob::is_running() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _state.running;
}

bool NovaTerrainBuildJob::is_finished() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _state.finished;
}

float NovaTerrainBuildJob::get_progress_ratio() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _state.ratio;
}

int NovaTerrainBuildJob::get_progress_current() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _state.current;
}

int NovaTerrainBuildJob::get_progress_total() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _state.total;
}

String NovaTerrainBuildJob::get_progress_message() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return String(_state.message.c_str());
}

String NovaTerrainBuildJob::get_progress_phase() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return String(_state.phase.c_str());
}

Error NovaTerrainBuildJob::get_result_error() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return _state.result_error;
}

String NovaTerrainBuildJob::get_result_message() const {
	std::lock_guard<std::mutex> lock(_mutex);
	return String(_state.message.c_str());
}

void NovaTerrainBuildJob::wait_for_completion() {
	_join_thread();
}

} // namespace godot
