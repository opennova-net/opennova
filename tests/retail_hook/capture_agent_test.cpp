#include <opennova/retail_hook/capture_agent.h>

#include <cstdint>
#include <cstdio>

namespace hook = opennova::retail_hook;

static int failures = 0;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); \
            ++failures; \
        } \
    } while (0)

struct Record {
    std::uint32_t sequence{};
};

int main() {
    hook::CaptureAgent<Record, 2> agent;
    CHECK(agent.try_capture(Record{10}));
    CHECK(agent.try_capture(Record{11}));
    CHECK(!agent.try_capture(Record{12}));

    hook::CaptureAgentStats stats = agent.stats();
    CHECK(stats.accepted == 2);
    CHECK(stats.dropped_full == 1);
    CHECK(stats.dropped_contended == 0);

    Record record{};
    CHECK(agent.try_pop(record));
    CHECK(record.sequence == 10);
    CHECK(agent.try_pop(record));
    CHECK(record.sequence == 11);
    CHECK(!agent.try_pop(record));

    CHECK(agent.try_capture(Record{13}));
    CHECK(agent.try_pop(record));
    CHECK(record.sequence == 13);

    std::printf("retail_hook_capture_agent: %s\n",
                failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
