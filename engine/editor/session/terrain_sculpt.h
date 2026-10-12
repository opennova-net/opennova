#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <editor/documents/terrain_heights_document.h>
#include <formats/cpt/trngen/terrain_region_bake.h>

namespace opennova::editor {

class SessionCore;

// The terrain sculpt's baker (ADR 0046 S25, the heights lane): the session's own thread that bakes the heights a
// stroke changed with the TrnGen port's region bake (formats/cpt/trngen/terrain_region_bake.h), so that every tile
// the stroke did not reach keeps its bytes and the remade ones are the bytes a whole bake of the sculpted input makes.
// A bake starts once a heights document holds regions whose bake is due and no gesture is open in it (its stroke
// ended); a new stroke on the file stops a bake running for it (its regions join the next one). Its result lands as
// part of the stroke's own step (HeightsBakeLanding: one Ctrl+Z takes the stroke and its bake) when the document is
// still in the state the bake started from; otherwise it is dropped and its regions stay due. The first bake of a file
// makes its bake state: a cold in-memory bake of the input its tiles were made from (a .cpt's atlas: D-TERRAIN-23),
// kept for the file's next bakes. Save and Play wait (settle). Single-threaded on the session's side: poll, interrupt
// and settle are the session thread's; only the bake runs on the worker.
class TerrainSculpt {
public:
	explicit TerrainSculpt(SessionCore &core);
	~TerrainSculpt();
	TerrainSculpt(const TerrainSculpt &) = delete;
	TerrainSculpt &operator=(const TerrainSculpt &) = delete;

	// Each poll: a bake that finished lands (or is dropped), and a heights document due a bake with no stroke open
	// starts one when none runs.
	void poll();
	// A stroke's first dab on `path`: a bake running for it stops.
	void interrupt(const std::string &path);
	// Save and Play: every bake due for the heights document at `path` made and landed now, the caller waiting. False,
	// with why, where a bake fails or cannot land (its regions stay due: the file is written with the sculpted heights
	// over its old tiles there).
	bool settle(const std::string &path, std::string &error);
	// Whether a bake runs or is due for `path`, and its words for the pane's bake line ("" none).
	std::string bake_words(const std::string &path) const;
	// The bakes made since the session started (a test's count).
	size_t bakes_landed() const { return landed_; }
	// Flatten's target for a stroke (the live height under its first sample, raw16), kept for its gesture.
	void remember_flatten(uint64_t gesture, int32_t raw);
	bool flatten_target(uint64_t gesture, int32_t &raw) const;

private:
	struct Job;
	// The job a document's state asks for now; null where nothing is due or it cannot bake (a blocked or read-only
	// document, one with no planes).
	std::unique_ptr<Job> make_job(const TerrainHeightsDocument &document) const;
	void start(std::unique_ptr<Job> job);
	// The job's bake, on whichever thread runs it.
	static void run(Job &job);
	// A finished job landed in its document, or dropped; true when it landed.
	bool land(Job &job);
	void work();

	SessionCore &core_;
	mutable std::mutex mutex_;
	std::condition_variable wake_;
	std::thread worker_;
	bool stopping_ = false;
	std::unique_ptr<Job> queued_;   // handed to the worker
	std::unique_ptr<Job> running_;  // the worker's
	std::unique_ptr<Job> finished_; // back from the worker, to land
	std::atomic<bool> cancel_{false};
	std::string running_path_;
	size_t landed_ = 0;
	// The bake state a dropped first bake made, kept for the file's next attempt (its identity and load, and the
	// input it was made from, must match).
	struct Seed {
		uint64_t identity = 0, generation = 0;
		RasterPlane input;
		std::shared_ptr<const trngen::TerrainBakeState> state;
	};
	std::vector<Seed> seeds_;
	std::vector<std::pair<uint64_t, int32_t>> flatten_; // the last few strokes' targets
};

// The quadrant lock flags of the first .trn (by its path) naming the .cpt `cpt_name` as its polytrn_polydata, from the
// session's view of the project's files; none where no .trn names it. `others` names each later .trn naming it with
// other flags (the conflict noted).
std::array<trngen::CornerLockFlags, 4> terrain_bake_locks(const SessionCore &core, const std::string &cpt_name,
                                                          std::vector<std::string> *others = nullptr);

} // namespace opennova::editor
