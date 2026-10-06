#include <editor/documents/wave_check.h>

#include <map>
#include <string>

#include <editor/assets/asset_registry.h>
#include <editor/import/wave_source.h>

namespace opennova::editor {

namespace {

class WaveCheck final : public ProjectCheck {
public:
	bool update(const ProjectCheckInput &input) override {
		std::map<std::string, Kept> kept;
		std::vector<Diagnostic> findings;
		for (const AssetEntry &entry : input.validation.scan.entries) {
			if (entry.kind != AssetKind::Wave) continue;
			const uint64_t stamp = input.files.stamp(entry.logical_name);
			const auto known = waves_.find(entry.relative_path);
			Kept wave;
			if (known != waves_.end() && known->second.stamp == stamp && stamp != 0) {
				wave = known->second;
			} else {
				std::vector<uint8_t> bytes;
				if (!input.files.read(entry.logical_name, bytes)) continue; // a file that does not read is the scan's
				wave.stamp = stamp;
				wave.check = wave_retail_check(bytes);
			}
			if (!wave.check.plays)
				findings.push_back(make_finding(CoreFinding::AssetWaveUnplayable, DiagnosticSeverity::Warning,
				                                "The game cannot play " + entry.logical_name + ": " + wave.check.why +
				                                        " Import it again from its source: the import writes it as the "
				                                        "game plays it.",
				                                entry.relative_path));
			kept.emplace(entry.relative_path, std::move(wave));
		}
		waves_ = std::move(kept);
		const bool moved = !same(findings, findings_);
		findings_ = std::move(findings);
		return moved;
	}
	const std::vector<Diagnostic> &findings() const override { return findings_; }
	void clear() override {
		waves_.clear();
		findings_.clear();
	}

private:
	struct Kept {
		uint64_t stamp = 0;
		WaveRetailCheck check;
	};
	static bool same(const std::vector<Diagnostic> &a, const std::vector<Diagnostic> &b) {
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (a[i].asset != b[i].asset || a[i].message != b[i].message) return false;
		return true;
	}
	std::map<std::string, Kept> waves_;
	std::vector<Diagnostic> findings_;
};

} // namespace

std::unique_ptr<ProjectCheck> make_wave_check() { return std::make_unique<WaveCheck>(); }

} // namespace opennova::editor
