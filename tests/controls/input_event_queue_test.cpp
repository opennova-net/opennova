// The input frame's event queue (D-CTRL-6, LI-s14): 32 records behind a count, the 33rd
// dropped; the drain takes the front record and shifts the rest down, so a record queued
// while the drain runs joins it at the back, and a reset (Input_ResetAllState's memset)
// leaves nothing for the drain. The role's drain over it is pinned in
// inmatch_tick_input_source.
#include <runtime/controls/input_event_queue.h>

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <vector>

using namespace opennova::controls;

namespace {

#define CHECK(condition) do { \
	if (!(condition)) { \
		std::cerr << __func__ << ':' << __LINE__ << ": " #condition "\n"; \
		return false; \
	} \
} while (false)

InputEventRecord record(int32_t code, bool sign = false,
		InputEventOrigin origin = InputEventOrigin::Device) {
	return InputEventRecord{code, 0, 0, 0, sign, origin};
}

// [orig: Input_QueueEvent @0x4979C0 (the cap `cmp count, 20h; jge` @0x4979C8..0x4979CB, the
//  stores @0x497A1A..0x497A67, the count's step @0x497AA4)]
bool test_the_33rd_record_is_dropped() {
	InputEventQueue queue;
	for (int32_t i = 0; i < 32; ++i) CHECK(queue.push(record(i)));
	CHECK(!queue.push(record(99)));
	CHECK(queue.size() == InputEventQueue::kCapacity && queue.size() == 32);
	CHECK(queue.at(0).code == 0 && queue.at(31).code == 31);
	return true;
}

// The front record first, its arguments, sign and origin kept; the rest shifted down.
// [orig: Input_DequeueEvent @0x497F30 (the code's low 31 bits @0x497F4A, the arguments
//  @0x497F51..0x497F6E, the sign @0x497F7A, the shift @0x497F95..0x497FB2, the count's step
//  @0x497FB6)]
bool test_the_drain_takes_the_front_record() {
	InputEventQueue queue;
	InputEventRecord out;
	CHECK(queue.empty() && !queue.pop(out));
	CHECK(queue.push(InputEventRecord{4, 7, 8, 9, false, InputEventOrigin::Console}));
	CHECK(queue.push(record(23, true)));
	CHECK(queue.push(record(201)));
	CHECK(queue.pop(out) && out.code == 4 && out.p1 == 7 && out.p2 == 8 && out.p3 == 9 &&
			out.origin == InputEventOrigin::Console);
	CHECK(queue.size() == 2 && queue.at(0).code == 23 && queue.at(0).sign);
	// A record queued while the drain runs joins its back.
	CHECK(queue.push(record(152)));
	std::vector<int32_t> codes;
	while (queue.pop(out)) codes.push_back(out.code);
	CHECK((codes == std::vector<int32_t>{23, 201, 152}));
	CHECK(queue.empty());
	// A drained queue takes 32 records again.
	for (int32_t i = 0; i < 32; ++i) CHECK(queue.push(record(i)));
	CHECK(!queue.push(record(32)));
	return true;
}

// A reset inside the drain leaves nothing behind the dispatched record.
// [orig: Input_ResetAllState @0x497ED0 (the memset @0x497EE6); Input_ProcessFrame @0x49D520
//  (the count test @0x49D5E0)]
bool test_a_reset_ends_the_drain() {
	InputEventQueue queue;
	for (const int32_t code : {201, 55, 203}) CHECK(queue.push(record(code)));
	InputEventRecord out;
	std::vector<int32_t> codes;
	while (queue.pop(out)) {
		codes.push_back(out.code);
		if (out.code == 55) queue.reset();
	}
	CHECK((codes == std::vector<int32_t>{201, 55}));
	CHECK(queue.empty() && queue.push(record(19)) && queue.size() == 1);
	return true;
}

} // namespace

int main() {
	bool ok = true;
	for (bool (*test)() : {test_the_33rd_record_is_dropped, test_the_drain_takes_the_front_record,
			test_a_reset_ends_the_drain})
		ok = test() && ok;
	if (!ok) return EXIT_FAILURE;
	std::cout << "controls_input_event_queue: ok\n";
	return EXIT_SUCCESS;
}
