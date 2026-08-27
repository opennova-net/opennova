// The attach-command seat-selection mirror (world/vehicle_attach.h
// predict_seat_selection): the witnessed weights, the three command filters,
// and the verdict ladder (occupied before the command filter, lowest weight
// wins, first on ties). [orig: Entity_FindBestSeatSlot @0x4351F0; the attach
// commands 123/124/125 (entity_commands.h)]
#include <cstdio>
#include <vector>

#include <runtime/world/entity.h>
#include <runtime/world/vehicle_attach.h>

using namespace opennova::world;

static int failures = 0;
#define CHECK(c)                                                                       \
    do {                                                                               \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

namespace {

std::vector<SeatCandidate> seats() {
    // sitex00, ctrlx00, drvrx00 — the shape the MCP mirror test used.
    return {{SeatType::Passenger, false}, {SeatType::Controller, false},
            {SeatType::Driver, false}};
}

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
    CHECK(!seat_selection_mode_for_command(99, mode));
}

void test_prediction_follows_the_original_command_rules() {
    std::vector<SeatVerdict> v;
    SeatSelectionMode mode = SeatSelectionMode::PassengerOnly;
    CHECK(predict_seat_selection(seats(), &mode, v) == 0); // 123: passenger-only
    CHECK(v[0] == SeatVerdict::kSelected && v[1] == SeatVerdict::kSkippedCommand &&
          v[2] == SeatVerdict::kSkippedCommand);
    mode = SeatSelectionMode::RejectController;
    CHECK(predict_seat_selection(seats(), &mode, v) == 2); // 124: driver before passenger
    CHECK(v[1] == SeatVerdict::kSkippedCommand && v[0] == SeatVerdict::kEligible);
    mode = SeatSelectionMode::Any;
    CHECK(predict_seat_selection(seats(), &mode, v) == 1); // 125: ctrlx by priority
    CHECK(v[1] == SeatVerdict::kSelected);
    CHECK(predict_seat_selection(seats(), nullptr, v) == -1); // not a mount command
    CHECK(v[0] == SeatVerdict::kSkippedCommand && v[2] == SeatVerdict::kSkippedCommand);
}

void test_occupied_precedes_the_command_filter_and_ties_take_the_first() {
    std::vector<SeatCandidate> s = {{SeatType::Controller, true}, {SeatType::None, true},
                                    {SeatType::Driver, false}, {SeatType::Driver, false}};
    std::vector<SeatVerdict> v;
    SeatSelectionMode mode = SeatSelectionMode::Any;
    CHECK(predict_seat_selection(s, &mode, v) == 2);
    CHECK(v[0] == SeatVerdict::kSkippedOccupied);
    CHECK(v[1] == SeatVerdict::kSkippedOccupied); // occupied wins over the None filter
    CHECK(v[2] == SeatVerdict::kSelected);
    CHECK(v[3] == SeatVerdict::kEligible);
    std::vector<SeatCandidate> none = {{SeatType::None, false}};
    CHECK(predict_seat_selection(none, &mode, v) == -1);
    CHECK(v[0] == SeatVerdict::kSkippedCommand);
    CHECK(predict_seat_selection({}, &mode, v) == -1);
    CHECK(v.empty());
}

} // namespace

int main() {
    test_weights_are_the_witnessed_table();
    test_command_modes();
    test_prediction_follows_the_original_command_rules();
    test_occupied_precedes_the_command_filter_and_ties_take_the_first();
    if (failures == 0) std::printf("seat_prediction_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
