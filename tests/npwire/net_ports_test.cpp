// The LAN host bind scan (D-NET-210) [orig: NapiUdpSocket_CreateAndBind
// @ 0x62d2a0 fed by the authority arm of CNapiNetwork_OpenTransportSocket
// @ 0x4c6aa2; clamp NapiSocket_ClampBufferParams @ 0x62e180]: attempt count,
// stepping, the wrap back to min, and the clamp edges.

#include <vector>

#include "common/test_expect.h"
#include "npwire/net_ports.h"

int main() {
	using opennova::kRetailLanPortMax;
	using opennova::kRetailLanPortMin;
	using opennova::lan_host_bind_ports;

	// The shipped cfg defaults: 20 tries, 32768..32787 in order.
	const std::vector<uint16_t> stock = lan_host_bind_ports(kRetailLanPortMin);
	TEST_EXPECT(stock.size() == 20u);
	TEST_EXPECT(stock.front() == 32768u);
	TEST_EXPECT(stock.back() == 32787u);
	for (size_t i = 1; i < stock.size(); ++i) {
		TEST_EXPECT(stock[i] == stock[i - 1] + 1u);
	}

	// An authored min inside the range scans up to the shipped max.
	const std::vector<uint16_t> tail = lan_host_bind_ports(32780);
	TEST_EXPECT(tail.size() == 8u);
	TEST_EXPECT(tail.front() == 32780u);
	TEST_EXPECT(tail.back() == 32787u);

	// max < min clamps to max = min: a single try.
	const std::vector<uint16_t> single = lan_host_bind_ports(40000);
	TEST_EXPECT(single.size() == 1u);
	TEST_EXPECT(single.front() == 40000u);

	// attempts = (max - min + 1) / step, first bind at min, stepping by delta.
	// (The wrap-to-min guard exists for the CLIENT arm's random start; from a
	// min start the integer attempt count keeps every retry within max.)
	const std::vector<uint16_t> stepped = lan_host_bind_ports(100, 119, 3);
	TEST_EXPECT(stepped.size() == 6u);
	TEST_EXPECT(stepped[0] == 100u && stepped[1] == 103u && stepped[5] == 115u);
	// attempts truncates: 5/3 = 1 -> the single min try.
	const std::vector<uint16_t> truncated = lan_host_bind_ports(100, 104, 3);
	TEST_EXPECT(truncated.size() == 1u && truncated[0] == 100u);

	// step 0 clamps to 1 (the witnessed guard).
	const std::vector<uint16_t> zero_step = lan_host_bind_ports(100, 101, 0);
	TEST_EXPECT(zero_step.size() == 2u && zero_step[1] == 101u);

	return 0;
}
