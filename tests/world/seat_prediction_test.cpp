// The attach-command seat selection's tables (world/vehicle_attach.h): the
// witnessed seat weights and the three command filters.
// [orig: Entity_FindBestSeatSlot @0x4351F0; the attach commands 123/124/125
// (entity_commands.h)]
#include <cstdio>

#include <runtime/world/entity.h>
#include <runtime/world/vehicle_attach.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

void test_weights_are_the_witnessed_table() {
    CHECK(seat_priority_weight(SeatType::Controller, true) == 0x2000);
    CHECK(seat_priority_weight(SeatType::Driver, true) == 0x2000);
    CHECK(seat_priority_weight(SeatType::Gunner, true) == 0x20000);
    CHECK(seat_priority_weight(SeatType::Passenger, true) == 0x200000);
    CHECK(seat_priority_weight(SeatType::Passenger, false) == 0x2000000);
}

void test_command_modes() {
    SeatSelectionMode mode;
    CHECK(seat_selection_mode_for_command(kCommandAttachPassengerOnly, mode) &&
          mode == SeatSelectionMode::PassengerOnly);
    CHECK(seat_selection_mode_for_command(kCommandAttachSkipController, mode) &&
          mode == SeatSelectionMode::RejectController);
    CHECK(seat_selection_mode_for_command(kCommandAttachAnySeat, mode) &&
          mode == SeatSelectionMode::Any);
    // A non-attach id leaves the caller's fallback in place.
    mode = SeatSelectionMode::PassengerOnly;
    CHECK(!seat_selection_mode_for_command(99, mode));
    CHECK(mode == SeatSelectionMode::PassengerOnly);
}

} // namespace

int main() {
    test_weights_are_the_witnessed_table();
    test_command_modes();
    if (failures == 0) std::printf("seat_prediction_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
