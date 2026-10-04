#include <net/npwire/cs_config.h>

namespace opennova {

void apply_cs_field(CsConfig &cfg, uint32_t slot, int32_t value) {
	switch (slot) {
	case 0: cfg.timeout_ms = value; break;
	case 1: cfg.recv_max_per_tick = value; break;
	case 4: cfg.idle_send_interval_ms = value; break;
	case 5: cfg.active_send_interval_ms = value; break;
	case 6: cfg.packet_queue_interval_ms = value; break;
	case 10: cfg.packet_queue_max = value; break;
	case 11: cfg.msg_out_max = value; break;
	case 13: cfg.max_packet_bytes = value; break;
	case 14: cfg.max_packets_per_tick = value; break;
	default: break;
	}
}

namespace {

CsConfig cs_config_from_fields(const std::vector<CsField> &fields) {
	CsConfig cfg;
	for (const CsField &field : fields)
		apply_cs_field(cfg, field.field_index, static_cast<int32_t>(field.value));
	return cfg;
}

} // namespace

// [orig: CNapiGameSession_InitNPConnection @0x4d3be0 — the template @0x4d3e1f..0x4d3f64]
CsConfig novaworld_service_cs_config(int32_t max_packet_bytes) {
	return cs_config_from_fields(novaworld_service_cs_fields(max_packet_bytes));
}

// [orig: CNapiNetwork_Init @0x4ca4a0 — the template @0x4caa81..0x4cac18]
CsConfig jointoperations_cs_config(int32_t max_packet_bytes) {
	return cs_config_from_fields(jointoperations_cs_fields(max_packet_bytes));
}

} // namespace opennova
