extends GutTest

# NovaUdpPump (engine/network/nova_udp_pump.{h,cpp}) — the raw-UDP socket plumbing for the
# co-op-LAN net path (Increment B). No sim, no protocol framing: this proves that a host can
# bind, a joiner can dial, and raw datagrams move BOTH directions over localhost with the
# correct source address surfaced (the address the host later keys a connection by).


func _pump():
	var p = NovaUdpPump.new()
	autofree(p)
	return p


# Poll a pump until it has buffered `want` datagrams or the tries run out (UDP on loopback can
# take a tick or two to deliver). Returns the number buffered.
func _poll_until(pump, want: int, tries: int = 100) -> int:
	for i in range(tries):
		pump.poll()
		if pump.inbound_count() >= want:
			break
		OS.delay_msec(2)
	return pump.inbound_count()


func test_fresh_pump_is_closed() -> void:
	var p = _pump()
	assert_false(p.is_open(), "a fresh pump holds no socket")
	assert_eq(p.inbound_count(), 0, "no inbound datagrams")


func test_host_bind_assigns_a_port() -> void:
	var host = _pump()
	assert_eq(host.bind_listen(0), OK, "host binds an OS-assigned port")
	assert_true(host.is_open(), "socket open after bind")
	assert_gt(host.local_port(), 0, "host got a real local port")
	host.close()
	assert_false(host.is_open(), "closed")


func test_joiner_to_host_roundtrip_with_source_address() -> void:
	var host = _pump()
	var join = _pump()
	assert_eq(host.bind_listen(0), OK)
	var host_port: int = host.local_port()
	assert_eq(join.dial("127.0.0.1", host_port), OK, "joiner dials the host")

	# Joiner -> host.
	var c2s := PackedByteArray([0x0C, 1, 2, 3])
	assert_eq(join.send_to_host(c2s), OK, "joiner sent a datagram")

	assert_eq(_poll_until(host, 1), 1, "host received exactly one datagram")
	var pkt: Dictionary = host.take_inbound()
	assert_eq(pkt.get("bytes"), c2s, "host received the joiner's bytes verbatim")
	assert_eq(String(pkt.get("ip")), "127.0.0.1", "host learned the joiner's ip")
	var joiner_port: int = int(pkt.get("port"))
	assert_gt(joiner_port, 0, "host learned the joiner's source port")
	assert_eq(host.inbound_count(), 0, "inbound drained after take")


func test_host_to_joiner_reply() -> void:
	var host = _pump()
	var join = _pump()
	assert_eq(host.bind_listen(0), OK)
	assert_eq(join.dial("127.0.0.1", host.local_port()), OK)

	# Joiner pings so the host learns its address.
	assert_eq(join.send_to_host(PackedByteArray([0x41])), OK)
	assert_eq(_poll_until(host, 1), 1, "host saw the joiner ping")
	var ping: Dictionary = host.take_inbound()
	var joiner_ip := String(ping.get("ip"))
	var joiner_port := int(ping.get("port"))

	# Host replies to the learned address.
	var s2c := PackedByteArray([0x0A, 9, 8, 7, 6, 5])
	assert_eq(host.send_to(joiner_ip, joiner_port, s2c), OK, "host replied to the joiner")
	assert_eq(_poll_until(join, 1), 1, "joiner received the reply")
	var got: Dictionary = join.take_inbound()
	assert_eq(got.get("bytes"), s2c, "joiner received the host's bytes verbatim")


func test_two_joiners_keep_distinct_source_ports() -> void:
	var host = _pump()
	var a = _pump()
	var b = _pump()
	assert_eq(host.bind_listen(0), OK)
	var host_port: int = host.local_port()
	assert_eq(a.dial("127.0.0.1", host_port), OK)
	assert_eq(b.dial("127.0.0.1", host_port), OK)

	assert_eq(a.send_to_host(PackedByteArray([1])), OK)
	assert_eq(b.send_to_host(PackedByteArray([2])), OK)
	assert_eq(_poll_until(host, 2), 2, "host received both joiners' datagrams")

	var ports := {}
	for i in range(2):
		var pkt: Dictionary = host.take_inbound()
		ports[int(pkt.get("port"))] = int(pkt.get("bytes")[0])
	assert_eq(ports.size(), 2, "two distinct source ports (one per joiner)")


func test_dialed_pump_accepts_only_the_resolved_host_endpoint() -> void:
	var host = _pump()
	var join = _pump()
	var spoof = _pump()
	assert_eq(host.bind_listen(0), OK)
	assert_eq(spoof.bind_listen(0), OK)
	# Deliberately dial by hostname: the retained endpoint must be canonicalized
	# so the host's numeric packet source still matches.
	assert_eq(join.dial("localhost", host.local_port()), OK)

	# Teach the real host where the joiner is listening.
	assert_eq(join.send_to_host(PackedByteArray([0x41])), OK)
	assert_eq(_poll_until(host, 1), 1)
	var ping: Dictionary = host.take_inbound()
	var joiner_ip := String(ping.get("ip"))
	var joiner_port := int(ping.get("port"))

	var forged := PackedByteArray([0xBA, 0xD0])
	var authentic := PackedByteArray([0x0A, 0x01])
	assert_eq(spoof.send_to(joiner_ip, joiner_port, forged), OK,
			"a second socket can address the joiner's UDP port")
	assert_eq(host.send_to(joiner_ip, joiner_port, authentic), OK)

	assert_eq(_poll_until(join, 1), 1, "the authentic host datagram is admitted")
	# Give the forged datagram time to reach the socket too. A dialed pump must
	# discard it at the UDP boundary instead of exposing it to protocol consumers.
	for i in range(20):
		join.poll()
		OS.delay_msec(2)
	assert_eq(join.inbound_count(), 1, "the non-host source port is rejected")
	assert_eq(join.take_inbound().get("bytes"), authentic,
			"the surviving datagram came from the dialed host")
