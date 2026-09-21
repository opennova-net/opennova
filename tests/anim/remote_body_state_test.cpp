#include <runtime/anim/remote_body_state.h>
#include "common/test_expect.h"
#include <initializer_list>

using namespace opennova::anim;

int main() {
    RemoteBodyState state;
    TEST_EXPECT(state.request(41, 0x4) == BodyArrival::commit);
    TEST_EXPECT(state.request(48, 0) == BodyArrival::queue);
    state.arm_completion(0.25, 1.0, true);
    TEST_EXPECT(state.request(41, 0x4) == BodyArrival::keep);
    TEST_EXPECT(state.pending() == 48 && state.needs_tick());
    TEST_EXPECT(!state.promote_if_due(0.99));
    // Replacing pending after the boundary cannot postpone the armed end.
    TEST_EXPECT(state.request(43, 0) == BodyArrival::queue);
    state.arm_completion(1.01, 1.0, true);
    TEST_EXPECT(state.promote_if_due(1.01));
    TEST_EXPECT(state.current() == 43 && !state.has_pending());

    state.reset();
    TEST_EXPECT(state.request(115, 0x20) == BodyArrival::commit);
    TEST_EXPECT(state.request(43, 0) == BodyArrival::queue);
    TEST_EXPECT(state.request(1, 0x1) == BodyArrival::commit);
    TEST_EXPECT(state.current() == 1 && !state.has_pending());
    TEST_EXPECT(body_arrival(41, 1, 0x24, 0x1) == BodyArrival::queue);
    TEST_EXPECT(body_arrival(1, 2, 0, 0) == BodyArrival::commit);
    TEST_EXPECT(body_arrival(1, -1, 0, 0) == BodyArrival::keep);

    for (const double length : {0.0, 0.5}) {
        state.reset();
        state.request(41, 0x4);
        state.request(48, 0);
        state.arm_completion(1.0, length, false);
        TEST_EXPECT(state.promote_if_due(1.0));
    }

    state.reset();
    state.request(1, 0);
    state.begin_blend(4, 0.25, 0, 0);
    TEST_EXPECT(state.advance_blend(1));
    TEST_EXPECT(state.source_phase() == 5 && state.target_phase() == 1);
    TEST_EXPECT(state.weight() == 0.1f);
    state.set_source_time(0.3);
    state.request(2, 0);
    state.begin_blend(80, 5.0, 6, 0x400);
    TEST_EXPECT(state.source_phase() == 5 && state.source_time() == 0.3);
    TEST_EXPECT(state.target_phase() == 6 && state.weight() == 0.0f);
    TEST_EXPECT(!state.advance_blend(99));
    for (int tick = 0; tick < 14; ++tick) {
        TEST_EXPECT(state.advance_blend(2));
        TEST_EXPECT(state.weight() < 1.0f);
    }
    TEST_EXPECT(state.advance_blend(2) && state.weight() == 1.0f);
    state.clear_blend();
    TEST_EXPECT(!state.needs_tick());
    state.reset();
    state.request(41, 0x4);
    state.request(48, 0);
    state.arm_completion(0.0, 1.0, true);
    state.request(0, 0);
    TEST_EXPECT(!state.has_pending());
    state.request(48, 0);
    state.arm_completion(2.0, 1.0, true);
    TEST_EXPECT(!state.promote_if_due(2.5));
    TEST_EXPECT(state.promote_if_due(3.0));
    state.reset();
    TEST_EXPECT(state.current() == -1 && !state.has_pending());
    return 0;
}
