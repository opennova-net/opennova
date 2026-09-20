#pragma once

// A non-zero random 32-bit id (client index / key, browse cookie) for the
// session bindings that open a wire conversation.

#include <cstdint>
#include <random>

namespace godot {

inline uint32_t pick_random_uint32() {
	static thread_local std::mt19937 gen{std::random_device{}()};
	return std::uniform_int_distribution<uint32_t>(1)(gen);
}

} // namespace godot
