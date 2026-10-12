// Shared script variable store.
//
// THE key shared-state seam between the two scripting systems: the original's
// mission-variable array g_WacMissionVars is written by BOTH WAC (`set(V#)`,
// WacScript_ResolveParameter resolves V# -> 4*idx+0xC6B240) AND the BMS event
// system (EventAction_Dispatch MisvarChange writes g_WacMissionVars[idx];
// EventTrigger_EvaluateCondition cat 4 compares it). They are literally the same
// storage, so a single ScriptVarStore backs both evaluators.
//
// Two banks. The 2048-byte mission bank at 0xC6B240 is V0..V255 in its
// first half and the compiler-declared VAR/ARRAY slots at 0xC6B640 + 4n in
// its second half (n = declaration order; the port maps declared name n to
// index 256 + n). WacScript_InitAndLoad zeroes ONLY the first half
// (memset 0x400 @0x4f95ee); the declared half has no zeroing writer in the
// image (Script_Compile's declaration arm stores name/address/type and never
// the slot, @0x4f3812..0x4f3964), so slot n keeps the previous compile's
// value across a restart or the next mission in one process. Globals G# at
// 0xC6BA40 are zeroed on every reachable load (the @0x4f963d gate).
// M# binds the active audio context at compilation, outside this store.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace opennova::world {

class ScriptVarStore {
public:
    static constexpr int kMissionVars = 512;      // V0..V255 + declared 256..511 [orig: 0xC6B240]
    static constexpr int kNumberedMissionVars = 256; // the V# half InitAndLoad zeroes
    static constexpr int kGlobalVars = 256;       // G0..G255  [orig: 0xC6BA40]

    // Values are raw 32-bit (16.16 fixed where the slot holds a scaled scalar),
    // matching the original's int storage.
    int32_t get_mission(int i) const { return in_range(i, kMissionVars) ? mission_[i] : 0; }
    void set_mission(int i, int32_t v) { if (in_range(i, kMissionVars)) mission_[i] = v; }

    int32_t get_global(int i) const { return in_range(i, kGlobalVars) ? global_[i] : 0; }
    void set_global(int i, int32_t v) { if (in_range(i, kGlobalVars)) global_[i] = v; }

    // WAC's own state, which retail keeps in the globals around the two banks
    // and the VM reads and writes here: per IF (the compiler's 1024-entry IF
    // space) its fired tick, ever-fired dword and active byte, per DO loop
    // (the 1024-entry DO space) its counter and choice bytes, the DORND seed
    // after them, and RND just below the mission bank.
    // [orig: g_WacEventFiredTick @0xC6BE40, g_WacEventEverFired @0xC6CE40,
    //  g_WacEventActive @0xC6DE40 - WacScript_ExecuteBytecode THEN @0x4F5995 /
    //  @0x4F599F / @0x4F59AA; g_WacLoopCounters @0xC6E240, g_WacLoopChoices
    //  @0xC6E640 - DO @0x4F5A20 / @0x4F5A48; g_WacRngSeed @0xC6EA40 - DORND
    //  @0x4F5A7E; g_WacVarRND @0xC6B23C - WacCmd_Random @0x4ED2C8; the IF and DO
    //  bounds Script_Compile @0x4F4A06 / @0x4F417E]
    static constexpr int kWacIfs = 1024;
    static constexpr int kWacDos = 1024;
    static constexpr uint32_t kDorndSeed = 0x12333333u; // [orig: WacScript_InitAndLoad @0x4F966B]
    // The arrays sit on the heap: a World is often a stack local, and 11 KB
    // more per World overflowed the 1 MB Windows stack of nested fixtures.
    struct WacState {
        std::vector<uint32_t> fired_tick = std::vector<uint32_t>(kWacIfs);
        std::vector<int32_t> ever_fired = std::vector<int32_t>(kWacIfs);
        std::vector<uint8_t> active = std::vector<uint8_t>(kWacIfs);
        std::vector<uint8_t> do_counter = std::vector<uint8_t>(kWacDos);
        std::vector<uint8_t> do_choice = std::vector<uint8_t>(kWacDos);
        uint32_t dornd_seed = kDorndSeed;
        int32_t rnd = 0;
    };
    WacState wac;

    // The BMS reads and writes of a mission variable (the cat-4 trigger,
    // MisvarChange) index the mission bank with no bound. The index scales by
    // 4 in a 32-bit address, so only its low 30 bits select the word, and the
    // words past the bank's 512 run on into the G bank, which follows it
    // directly (0xC6B240 + 4 * 512 = 0xC6BA40), then into WAC's state: words
    // 768..1791 the fired ticks, 1792..2815 the ever-fired dwords, 2816..3071
    // the active bytes, 3072..3327 the DO counters, 3328..3583 the DO choices
    // (each byte array four entries to a word, the lowest in the low byte),
    // 3584 the DORND seed, and word 0x3FFFFFFF (index -1) RND. Every other
    // word, which a small negative index also selects, lands in other globals
    // (D-EVT-14): read 0 and dropped here. WAC operands never get there: the
    // compiler holds V# and G# under 256 and the declared slots to 256.
    // [orig: EventTrigger_EvaluateCondition @0x453620 - `mov ecx,
    //  g_WacMissionVars[eax*4]` @0x453AAC; EventAction_Dispatch @0x4542E0, case 5
    //  - the Set @0x45439A; WacScript_ResolveParameter @0x4F2920 - G# =
    //  0xC6BA40 + 4n @0x4F2B66, the V# and G# bounds @0x4F2AC0 / @0x4F2B27;
    //  Script_Compile @0x4F31F0 - the declared-slot bound @0x4F3812]
    int32_t get_bank_word(int32_t index) const {
        uint32_t word = bank_word(index);
        if (word == kRndWord) return wac.rnd;
        if (word < uint32_t(kMissionVars)) return mission_[word];
        word -= kMissionVars;
        if (word < uint32_t(kGlobalVars)) return global_[word];
        word -= kGlobalVars;
        if (word < uint32_t(kWacIfs)) return static_cast<int32_t>(wac.fired_tick[word]);
        word -= kWacIfs;
        if (word < uint32_t(kWacIfs)) return wac.ever_fired[word];
        word -= kWacIfs;
        if (word < uint32_t(kWacIfs / 4)) return packed_word(wac.active, word);
        word -= kWacIfs / 4;
        if (word < uint32_t(kWacDos / 4)) return packed_word(wac.do_counter, word);
        word -= kWacDos / 4;
        if (word < uint32_t(kWacDos / 4)) return packed_word(wac.do_choice, word);
        word -= kWacDos / 4;
        if (word == 0) return static_cast<int32_t>(wac.dornd_seed);
        return 0;
    }
    void set_bank_word(int32_t index, int32_t v) {
        uint32_t word = bank_word(index);
        if (word == kRndWord) { wac.rnd = v; return; }
        if (word < uint32_t(kMissionVars)) { mission_[word] = v; return; }
        word -= kMissionVars;
        if (word < uint32_t(kGlobalVars)) { global_[word] = v; return; }
        word -= kGlobalVars;
        if (word < uint32_t(kWacIfs)) { wac.fired_tick[word] = static_cast<uint32_t>(v); return; }
        word -= kWacIfs;
        if (word < uint32_t(kWacIfs)) { wac.ever_fired[word] = v; return; }
        word -= kWacIfs;
        if (word < uint32_t(kWacIfs / 4)) { set_packed_word(wac.active, word, v); return; }
        word -= kWacIfs / 4;
        if (word < uint32_t(kWacDos / 4)) { set_packed_word(wac.do_counter, word, v); return; }
        word -= kWacDos / 4;
        if (word < uint32_t(kWacDos / 4)) { set_packed_word(wac.do_choice, word, v); return; }
        word -= kWacDos / 4;
        if (word == 0) wac.dornd_seed = static_cast<uint32_t>(v);
    }

    // WacScript_InitAndLoad's clears of WAC's state: the fired ticks, the
    // DO counters, the ever-fired dwords and the active bytes, then the DORND
    // seed. No load clears the DO choices or RND.
    // [orig: WacScript_InitAndLoad @0x4F91F0 - the memsets @0x4F95F9 /
    //  @0x4F9609 / @0x4F9619 / @0x4F9629, the seed @0x4F966B]
    void reset_wac_load_state() {
        std::fill(wac.fired_tick.begin(), wac.fired_tick.end(), 0u);
        std::fill(wac.do_counter.begin(), wac.do_counter.end(), uint8_t(0));
        std::fill(wac.ever_fired.begin(), wac.ever_fired.end(), 0);
        std::fill(wac.active.begin(), wac.active.end(), uint8_t(0));
        wac.dornd_seed = kDorndSeed;
    }

    // The per-load clear: V0..V255 only. The declared half (256..511) and
    // the globals are untouched, as in the original's load.
    // [orig: WacScript_InitAndLoad memset(g_WacMissionVars, 0, 0x400) @0x4f95ee]
    void clear_numbered_mission_vars() {
        for (int i = 0; i < kNumberedMissionVars; ++i) mission_[i] = 0;
    }
    // Process-start state: every bank zero.
    void clear_all() { mission_.fill(0); global_.fill(0); }

    // Carry the declared half (256..511) of a previous mission's store into
    // this one: the retail bank is process-global and no load path zeroes
    // those slots, so a rebuilt kernel reads slot n at the value the previous
    // compile's slot n held. V# (zeroed per load) and G# (zeroed on every
    // reachable load) are NOT carried.
    // [orig: Script_Compile declaration arm @0x4f3812..0x4f3964 -- valueAddress
    //  = 0xC6B640 + 4n, slot never initialised; no zeroing writer in the image]
    void carry_declared_from(const ScriptVarStore &prev) {
        for (int i = kNumberedMissionVars; i < kMissionVars; ++i) mission_[i] = prev.mission_[i];
    }

    // Carry the previous mission's DO choices and RND: no mission load stores to
    // either (the mission start's WAC load clears the IF state and the DO
    // counters and seeds DORND, reset_wac_load_state), so the next mission
    // reads both at the previous run's values until it writes them. A
    // save-game load copies the choices from its file but not RND, which lies
    // below the copied run (unported). The restart's rewind restores the
    // sealed start instead (D-EVT-15).
    // [orig: g_WacLoopChoices @0xC6E640, written by WacScript_ExecuteBytecode
    //  alone (@0x4F5A48 / @0x4F5AD0 / @0x4F5AF0) and the bank words;
    //  g_WacVarRND @0xC6B23C, stored by WacCmd_Random @0x4ED2C8;
    //  WacScript_LoadVarState @0x4EE520, the 0x38AC-byte copy to 0xC6B240
    //  called @0x4EE52F]
    void carry_wac_kept_from(const ScriptVarStore &prev) {
        wac.do_choice = prev.wac.do_choice;
        wac.rnd = prev.wac.rnd;
    }

private:
    static bool in_range(int i, int n) { return i >= 0 && i < n; }
    // The index's low 30 bits, 0..0x3FFFFFFF.
    static uint32_t bank_word(int32_t index) {
        return static_cast<uint32_t>(index) & 0x3FFFFFFFu;
    }
    static constexpr uint32_t kRndWord = 0x3FFFFFFFu; // 0xC6B240 - 4
    // A byte array read as dwords: four entries to a word, little-endian.
    static int32_t packed_word(const std::vector<uint8_t> &bytes, uint32_t word) {
        const size_t at = size_t(word) * 4;
        return static_cast<int32_t>(uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 |
                uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24);
    }
    static void set_packed_word(std::vector<uint8_t> &bytes, uint32_t word, int32_t v) {
        const size_t at = size_t(word) * 4;
        for (size_t i = 0; i < 4; ++i)
            bytes[at + i] = static_cast<uint8_t>(static_cast<uint32_t>(v) >> (8 * i));
    }
    std::array<int32_t, kMissionVars> mission_{};
    std::array<int32_t, kGlobalVars> global_{};
};

} // namespace opennova::world
