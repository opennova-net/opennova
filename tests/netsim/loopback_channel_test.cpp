// LoopbackChannel — FIFO ordering + directionality (the in-process transport-mode-1
// byte path, ADR 0011 Decision 2).

#include "netsim/loopback_channel.h"

#include <cstdio>

namespace {

bool expect(bool cond, const char *msg) {
	if (cond) return true;
	std::fprintf(stderr, "FAIL: %s\n", msg);
	return false;
}

bool check_s2c_fifo_order() {
	opennova::netsim::LoopbackChannel ch;
	ch.host_send(0x0A, {1, 2, 3});
	ch.host_send(0x10, {4});
	ch.host_send(0x0A, {5, 6});
	if (!expect(ch.s2c_pending() == 3, "three S2C datagrams queued")) return false;
	// The host side never sees its own S2C traffic.
	opennova::netsim::Datagram dg;
	if (!expect(!ch.host_recv(dg), "host_recv finds no C2S")) return false;

	if (!expect(ch.client_recv(dg) && dg.tag == 0x0A && dg.body.size() == 3 &&
	            dg.body[0] == 1 && dg.body[2] == 3, "first S2C in order")) return false;
	if (!expect(ch.client_recv(dg) && dg.tag == 0x10 && dg.body.size() == 1 &&
	            dg.body[0] == 4, "second S2C in order")) return false;
	if (!expect(ch.client_recv(dg) && dg.tag == 0x0A && dg.body.size() == 2 &&
	            dg.body[1] == 6, "third S2C in order")) return false;
	if (!expect(!ch.client_recv(dg) && ch.s2c_pending() == 0, "S2C drained")) return false;
	return true;
}

bool check_c2s_direction() {
	opennova::netsim::LoopbackChannel ch;
	ch.client_send(0x0C, {7, 8});
	if (!expect(ch.c2s_pending() == 1, "one C2S queued")) return false;
	opennova::netsim::Datagram dg;
	if (!expect(!ch.client_recv(dg), "client_recv finds no S2C")) return false;
	if (!expect(ch.host_recv(dg) && dg.tag == 0x0C && dg.body.size() == 2 &&
	            dg.body[0] == 7, "host_recv pulls the C2S")) return false;
	if (!expect(!ch.host_recv(dg), "C2S drained")) return false;
	return true;
}

bool check_clear() {
	opennova::netsim::LoopbackChannel ch;
	ch.host_send(0x0A, {1});
	ch.client_send(0x0C, {2});
	ch.clear();
	if (!expect(ch.s2c_pending() == 0 && ch.c2s_pending() == 0, "clear empties both FIFOs")) return false;
	return true;
}

} // namespace

int main() {
	bool ok = true;
	ok = check_s2c_fifo_order() && ok;
	ok = check_c2s_direction() && ok;
	ok = check_clear() && ok;
	std::fprintf(stderr, ok ? "OK\n" : "FAIL\n");
	return ok ? 0 : 1;
}
