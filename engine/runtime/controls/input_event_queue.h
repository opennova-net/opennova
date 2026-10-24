#pragma once

#include <cstddef>
#include <cstdint>

namespace opennova::controls {

// One record of the input frame's event queue: the dispatch code, the three arguments the
// queueing pass hands over and the sign bit (bit 31 of the queued code, the fire's Shift
// sign, which the dequeue splits off). The key scan queues its rows with the three
// arguments 0. A record carries nothing of what queued it: a key's and a console line's
// walk's alike reach the one dispatcher (inmatch::Role::dispatch_record).
// [orig: Input_QueueEvent @0x4979C0 (the stores @0x497A1A / @0x497A34 / @0x497A4E /
//  @0x497A67); Input_DequeueEvent @0x497F30 (the code's low 31 bits @0x497F4A, the
//  arguments @0x497F51..0x497F6E, the sign `shr 1Fh` @0x497F7A); Input_ProcessKeyboardEvents
//  @0x49D1F0 (the scan's queues @0x49D428..0x49D42F and @0x49D47F..0x49D488, the sign
//  @0x49D470 OR'd @0x49D483)]
struct InputEventRecord {
	int32_t code = 0;
	int32_t p1 = 0;
	int32_t p2 = 0;
	int32_t p3 = 0;
	bool sign = false;
};

// g_InputEventQueueStorage: 32 records behind a count. A push at a count of 32 is dropped
// (its two flag gates are dead: no writer sets dword_B3EA30 to 1, and no catalog row carries
// 0x2000000). The drain dequeues the front record and shifts the rest down, then dispatches
// it, until the count reads 0: a record a dispatch queues joins the same drain, and a reset
// inside a dispatch ends it. Input_ResetAllState's memset is reset().
// [orig: Input_QueueEvent @0x4979C0 (the cap `cmp count, 20h; jge` @0x4979C8..0x4979CB, the
//  gates @0x4979D1..0x497A0B, the count's step @0x497AA4); Input_DequeueEvent @0x497F30 (the
//  shift @0x497F95..0x497FB2, the count's step @0x497FB6); Input_ProcessFrame @0x49D520 (the
//  drain @0x49D599..0x49D5E7: the count test @0x49D599, Input_DequeueEvent @0x49D5BA,
//  Input_HandleActionBinding @0x49D5D8, the count test @0x49D5E0); Input_ResetAllState
//  @0x497ED0 (the 0x384-byte memset @0x497EE6)]
class InputEventQueue {
public:
	static constexpr std::size_t kCapacity = 32;

	bool push(const InputEventRecord &record) {
		if (count_ >= kCapacity) return false;
		records_[count_++] = record;
		return true;
	}
	// The front record, the rest shifted down; false on an empty queue.
	bool pop(InputEventRecord &out) {
		if (count_ == 0) return false;
		out = records_[0];
		for (std::size_t i = 0; i + 1 < count_; ++i) records_[i] = records_[i + 1];
		--count_;
		return true;
	}
	void reset() { count_ = 0; }
	std::size_t size() const { return count_; }
	bool empty() const { return count_ == 0; }
	const InputEventRecord &at(std::size_t index) const { return records_[index]; }

private:
	InputEventRecord records_[kCapacity];
	std::size_t count_ = 0;
};

} // namespace opennova::controls
