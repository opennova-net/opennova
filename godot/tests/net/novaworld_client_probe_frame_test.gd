extends GutTest

# The NovaWorld menu's probe frame (D-NET-571): retail's menu frame pumps its session ahead of
# the state's case, so the frame whose state 1 starts the gate probe reads nothing after it and
# the gate's answer is read from the next frame's pump on (engine: NwuLobbySession::probe).
# NovaWorldClient skips the lobby's tick in the process frame that ran start(). An activation in
# the input step (a key, a button) runs before the frame's process step, which then reaches the
# new client; the test's code resumed from the tree's process_frame signal stands in the same
# place, ahead of the step. An activation in another node's process (the menu shell's mouse pump)
# reaches the new client a frame later, the step walking the node list it began with. Either way
# the gate socket is first read in the frame after start()'s. The fake gate answers the probe
# with three bytes that are no envelope, so the lobby's first read of the gate socket reports
# "bad gate envelope" (error_occurred) in the frame it runs.


## Runs `action` once from its own _process: the menu shell's mouse pump's place in the step.
class ProcessStepCaller extends Node:
	var action: Callable

	func _process(_delta: float) -> void:
		if action.is_valid():
			var run := action
			action = Callable()
			run.call()


var _gate: PacketPeerUDP
var _client: NovaWorldClient
var _error_frames: Array = []
var _error_messages: Array = []
var _start_frame := -1


func before_each() -> void:
	_gate = PacketPeerUDP.new()
	assert_eq(_gate.bind(0, "127.0.0.1"), OK, "the fake gate binds on loopback")
	_client = null
	_error_frames.clear()
	_error_messages.clear()
	_start_frame = -1


func after_each() -> void:
	if is_instance_valid(_client):
		_client.stop()
	_client = null
	if _gate != null:
		_gate.close()
	_gate = null


func _on_client_error(message: String) -> void:
	_error_frames.append(Engine.get_process_frames())
	_error_messages.append(message)


# A new client under the test, its probe aimed at the fake gate, started; then the fake gate
# answers the probe's source.
func _start_client() -> void:
	_client = NovaWorldClient.new()
	_client.host = "127.0.0.1"
	_client.gate_port = _gate.get_local_port()
	_client.error_occurred.connect(_on_client_error)
	add_child_autofree(_client)
	_client.start()
	_start_frame = Engine.get_process_frames()
	assert_eq(_client.get_state(), NovaWorldClient.STATE_GATE_PROBING,
			"start() sent the gate probe")
	assert_true(_answer_probe(), "the fake gate took the probe and answered it")


# The probe left in start(); loopback holds it at once (a bounded wait inside this frame).
func _answer_probe() -> bool:
	var waited := 0
	while _gate.get_available_packet_count() == 0 and waited < 200:
		OS.delay_usec(1000)
		waited += 1
	if _gate.get_available_packet_count() == 0:
		return false
	_gate.get_packet()
	var port := _gate.get_packet_port()
	_gate.set_dest_address("127.0.0.1", port)
	var sent := _gate.put_packet(PackedByteArray([0xDE, 0xAD, 0x01])) == OK
	OS.delay_usec(1000)
	return sent


func _assert_read_in_the_next_frame() -> void:
	assert_eq(_error_frames.size(), 1, "the gate socket was read once")
	if _error_frames.size() == 1:
		assert_eq(_error_frames[0], _start_frame + 1,
				"the first lobby tick is the frame after start()'s")
		assert_eq(_error_messages[0], "bad gate envelope")


func test_an_input_step_activation_ticks_the_lobby_from_the_next_frame() -> void:
	# Ahead of this frame's process step, where a key's or a button's activation runs.
	await get_tree().process_frame
	_start_client()
	await get_tree().process_frame
	assert_eq(_error_frames.size(), 0, "the probe's frame runs no lobby tick")
	await get_tree().process_frame
	_assert_read_in_the_next_frame()


func test_a_process_step_activation_ticks_the_lobby_from_the_next_frame() -> void:
	var caller := ProcessStepCaller.new()
	caller.action = _start_client
	add_child_autofree(caller)
	var frames := 0
	while _start_frame < 0 and frames < 10:
		await get_tree().process_frame
		frames += 1
	assert_gte(_start_frame, 0, "the caller's process ran start()")
	# Ahead of the process step of the frame after start()'s.
	assert_eq(_error_frames.size(), 0, "the probe's frame runs no lobby tick")
	await get_tree().process_frame
	_assert_read_in_the_next_frame()
