#include <runtime/devtools/net_window.h>

#include <runtime/devtools/debug_control_ids.h>

#include <imgui.h>

#include <cstdio>

namespace opennova::devtools {

namespace {

std::string bytes_text(uint64_t bytes) {
	char buf[32];
	if (bytes >= 1024ull * 1024ull) {
		std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
	} else if (bytes >= 1024ull) {
		std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
	} else {
		std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
	}
	return buf;
}

}  // namespace

void NetWindow::on_visibility(bool visible) {
	if (!visible) {
		snapshot_ = NetSnapshot{};
		previous_ = NetSnapshot{};
		format();
	}
}

void NetWindow::wanted_controls(std::vector<const char *> &out) const {
	out.push_back(control_id::kNetJoinerDiagnostics);
}

void NetWindow::set_snapshot(NetSnapshot snapshot) {
	previous_ = std::move(snapshot_);
	snapshot_ = std::move(snapshot);
	format();
}

bool NetWindow::take_request(ControlRequest &request) {
	if (requests_.empty()) return false;
	request = requests_.front();
	requests_.pop_front();
	return true;
}

void NetWindow::format() {
	session_.clear();
	traffic_.clear();
	if (!snapshot_.valid) return;
	const NetSnapshot &s = snapshot_;
	char buf[256];
	// The retail page's line: the frame rate and the quantum (the joiner's
	// send holdoff; the server's per-peer holdoff is in the peers table).
	std::snprintf(buf, sizeof(buf), "%s, %s | %.0f fps | bank %s | last frame %d tick(s) in %.2f ms",
			status_role_label(s.role), status_state_label(s.state), s.fps, s.bank_policy.c_str(),
			s.frame_ticks, static_cast<double>(s.frame_tick_us) / 1000.0);
	session_ = buf;
	if (!s.traffic_valid) {
		traffic_ = "no socket (a local session sends nothing)";
		return;
	}
	const NetTraffic &t = s.traffic;
	std::string rates;
	if (previous_.valid && previous_.traffic_valid && s.wall_seconds > previous_.wall_seconds) {
		const double dt = s.wall_seconds - previous_.wall_seconds;
		std::snprintf(buf, sizeof(buf), " | now %.1f/%.1f pkt/s, %s/s up, %s/s down",
				static_cast<double>(t.tx_packets - previous_.traffic.tx_packets) / dt,
				static_cast<double>(t.rx_packets - previous_.traffic.rx_packets) / dt,
				bytes_text(static_cast<uint64_t>(static_cast<double>(t.tx_bytes - previous_.traffic.tx_bytes) / dt)).c_str(),
				bytes_text(static_cast<uint64_t>(static_cast<double>(t.rx_bytes - previous_.traffic.rx_bytes) / dt)).c_str());
		rates = buf;
	}
	std::snprintf(buf, sizeof(buf), "sent %llu pkt (%s), received %llu pkt (%s)",
			static_cast<unsigned long long>(t.tx_packets), bytes_text(t.tx_bytes).c_str(),
			static_cast<unsigned long long>(t.rx_packets), bytes_text(t.rx_bytes).c_str());
	traffic_ = std::string(buf) + rates;
}

void NetWindow::draw(ImGuiPass &pass, uint64_t frame_index) {
	(void)pass;
	(void)frame_index;
	if (!snapshot_.valid) {
		ImGui::TextUnformatted("No session pushed (load a mission).");
		return;
	}
	const NetSnapshot &s = snapshot_;
	ImGui::TextUnformatted(session_.c_str());
	ImGui::TextUnformatted(traffic_.c_str());

	if (!s.peers.empty() || s.role == StatusRole::ListenServer || s.role == StatusRole::DedicatedServer) {
		ImGui::SeparatorText("Peers");
		if (s.peers.empty()) {
			ImGui::TextDisabled("No remote peers connected.");
		} else if (ImGui::BeginTable("peers", 8,
						   ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("slot");
			ImGui::TableSetupColumn("name / address");
			ImGui::TableSetupColumn("phase");
			ImGui::TableSetupColumn("rtt (avg)");
			ImGui::TableSetupColumn("quality");
			ImGui::TableSetupColumn("strikes");
			ImGui::TableSetupColumn("holdoff");
			ImGui::TableSetupColumn("traffic up/down");
			ImGui::TableHeadersRow();
			for (const NetPeerSnapshotRow &p : s.peers) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%d", p.slot);
				ImGui::TableNextColumn();
				ImGui::Text("%s\n%s", p.name.c_str(), p.address.c_str());
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(p.phase.c_str());
				ImGui::TableNextColumn();
				ImGui::Text("%u ms (%u)", p.rtt_ms, p.rtt_average_ms);
				if (p.receive_inactive_ms > 1000) {
					ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "silent %.1f s",
							static_cast<double>(p.receive_inactive_ms) / 1000.0);
				}
				ImGui::TableNextColumn();
				ImGui::Text("%d / 4", p.quality);
				ImGui::TableNextColumn();
				ImGui::Text("%d / %d", p.min_ping_strikes, p.max_ping_strikes);
				ImGui::TableNextColumn();
				ImGui::Text("%u / %u", p.send_holdoff_countdown, p.send_holdoff_ticks);
				ImGui::TableNextColumn();
				if (p.traffic_valid) {
					ImGui::Text("%s / %s", bytes_text(p.traffic.tx_bytes).c_str(),
							bytes_text(p.traffic.rx_bytes).c_str());
				} else {
					ImGui::TextDisabled("-");
				}
			}
			ImGui::EndTable();
		}
	}

	if (s.joiner.present) {
		const NetJoinerSnapshot &j = s.joiner;
		ImGui::SeparatorText("Joiner");
		ImGui::Text("stage %s%s%s", j.stage.c_str(), j.in_match ? " | in match" : "",
				j.deployed ? " | deployed" : "");
		ImGui::Text("ping %u ms (avg %u, session %u) | quality %d / 4 | send holdoff %u / %u",
				j.ping_ms, j.average_ping_ms, j.session_ping_ms, j.quality, j.send_holdoff_countdown,
				j.send_holdoff_ticks);
		ImGui::Text("frontier %u | outbound %u | records %llu | gap depth %u | retained %u",
				j.frontier_seq, j.outbound_seq, static_cast<unsigned long long>(j.records_applied),
				j.gap_depth, j.retained_outbound);
		if (j.freeze_suspected) {
			ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "freeze suspected (%d s flat)",
					j.flat_seconds);
		}
		ImGui::Text("challenges: entity CRC %u/%u, loadout CRC %u/%u, charattr %u (%u missing), clears %u",
				j.entity_checksum_answered, j.entity_checksum_seen, j.loadout_crc_answered,
				j.loadout_crc_seen, j.charattr_seen, j.charattr_row_missing, j.property_clears);
		if (!j.last_reject.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "last reject: %s", j.last_reject.c_str());
		}
		if (!j.last_disconnect.empty()) {
			ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.35f, 1.0f), "last disconnect: %s",
					j.last_disconnect.c_str());
		}
		draw_control(board_, control_id::kNetJoinerDiagnostics, requests_);
	}
}

}  // namespace opennova::devtools
