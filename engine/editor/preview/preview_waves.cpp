#include <editor/preview/preview_waves.h>

#include <chrono>
#include <filesystem>
#include <system_error>
#include <utility>

#include <base/io/file_time.h>
#include <editor/project/project_files.h>

namespace opennova::editor {

PreviewWaves::DiskStamp PreviewWaves::stamp_of(const std::string &file) {
	DiskStamp stamp;
	std::error_code ec;
	const std::filesystem::path path = path_of(file);
	if (!std::filesystem::is_regular_file(path, ec) || ec) return stamp;
	const uint64_t size = std::filesystem::file_size(path, ec);
	if (ec) return stamp;
	const int64_t written = io::file_modified_ticks(path);
	if (written == 0) return stamp; // its last write unread
	stamp.exists = true;
	stamp.size = size;
	stamp.written = written;
	return stamp;
}

PreviewWaves::Wave PreviewWaves::wave(const std::string &file) {
	Entry &entry = waves_[file];
	if (entry.serial == 0) {
		// Stamped before it is read: a write racing the read shows as a change at the next refresh.
		entry.stamp = stamp_of(file);
		entry.serial = ++serials_;
		entry.job = std::async(std::launch::async, [file]() {
			Decode out;
			std::vector<uint8_t> bytes;
			if (!io::read_file_bytes(file, bytes, out.error)) return out;
			out.decoded = lwf::wav_decode_pcm16(bytes.data(), bytes.size(), out.pcm, out.error);
			return out;
		});
	}
	if (!entry.finished && entry.job.valid() &&
			entry.job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
		entry.done = entry.job.get();
		entry.finished = true;
	}
	Wave out;
	out.serial = entry.serial;
	if (!entry.finished) return out;
	out.state = entry.done.decoded ? State::Decoded : State::Failed;
	out.pcm = entry.done.decoded ? &entry.done.pcm : nullptr;
	out.error = entry.done.error;
	return out;
}

std::vector<std::string> PreviewWaves::refresh() {
	std::vector<std::string> dropped;
	for (auto it = waves_.begin(); it != waves_.end();) {
		if (stamp_of(it->first) == it->second.stamp) {
			++it;
			continue;
		}
		dropped.push_back(it->first);
		it = waves_.erase(it);
	}
	return dropped;
}

int PreviewWaves::decoding() const {
	int count = 0;
	for (const auto &[file, entry] : waves_) count += entry.finished ? 0 : 1;
	return count;
}

// --- PreviewVoiceQueue -----------------------------------------------------------------------------

void PreviewVoiceQueue::add(const std::string &root, const std::vector<WorkspaceView::Voice> &voices) {
	for (const WorkspaceView::Voice &voice : voices) {
		Ready waiting;
		waiting.file = join_path(root, voice.path);
		waiting.voice = voice;
		(void)waves_.wave(waiting.file); // its decode begun
		waiting_.push_back(std::move(waiting));
	}
}

std::vector<PreviewVoiceQueue::Ready> PreviewVoiceQueue::take_ready() {
	std::vector<Ready> ready;
	std::vector<Ready> still;
	for (Ready &waiting : waiting_) {
		const PreviewWaves::Wave wave = waves_.wave(waiting.file);
		if (wave.state == PreviewWaves::State::Failed) continue;
		(wave.state == PreviewWaves::State::Decoded ? ready : still).push_back(std::move(waiting));
	}
	waiting_ = std::move(still);
	return ready;
}

std::vector<std::string> PreviewVoiceQueue::refresh() {
	std::vector<std::string> dropped = waves_.refresh();
	if (dropped.empty()) return dropped;
	std::vector<Ready> still;
	for (Ready &waiting : waiting_) {
		bool gone = false;
		for (const std::string &file : dropped) gone = gone || file == waiting.file;
		if (!gone) still.push_back(std::move(waiting));
	}
	waiting_ = std::move(still);
	return dropped;
}

} // namespace opennova::editor
