// Shared script variable store.
//
// THE key shared-state seam between the two scripting systems: the original's
// mission-variable array dword_C6B240 is written by BOTH WAC (`set(V#)`,
// WacScript_ResolveParameter resolves V# -> 4*idx+0xC6B240) AND the BMS event
// system (EventAction_Dispatch MisvarChange writes dword_C6B240[idx];
// EventTrigger_EvaluateCondition cat 4 compares it). They are literally the same
// storage, so a single ScriptVarStore backs both evaluators.
//
// Three banks. The 2048-byte mission bank at 0xC6B240 is V0..V255 in its
// first half and the compiler-declared VAR/ARRAY slots at 0xC6B640 + 4n in
// its second half (n = declaration order; the port maps declared name n to
// index 256 + n). WacScript_InitAndLoad zeroes ONLY the first half
// (memset 0x400 @0x4f95ee); the declared half has no zeroing writer in the
// image (Script_Compile's declaration arm stores name/address/type and never
// the slot, @0x4f3812..0x4f3964), so slot n keeps the previous compile's
// value across a restart or the next mission in one process. Globals G# at
// 0xC6BA40 are zeroed on every reachable load (the @0x4f963d gate); music
// M# is a small parallel bank.
#pragma once

#include <array>
#include <cstdint>

namespace opennova::world {

class ScriptVarStore {
public:
    static constexpr int kMissionVars = 512;      // V0..V255 + declared 256..511 [orig: 0xC6B240]
    static constexpr int kNumberedMissionVars = 256; // the V# half InitAndLoad zeroes
    static constexpr int kGlobalVars = 256;       // G0..G255  [orig: 0xC6BA40]
    static constexpr int kMusicVars = 16;         // M0..M15

    // Values are raw 32-bit (16.16 fixed where the slot holds a scaled scalar),
    // matching the original's int storage.
    int32_t get_mission(int i) const { return in_range(i, kMissionVars) ? mission_[i] : 0; }
    void set_mission(int i, int32_t v) { if (in_range(i, kMissionVars)) mission_[i] = v; }

    int32_t get_global(int i) const { return in_range(i, kGlobalVars) ? global_[i] : 0; }
    void set_global(int i, int32_t v) { if (in_range(i, kGlobalVars)) global_[i] = v; }

    int32_t get_music(int i) const { return in_range(i, kMusicVars) ? music_[i] : 0; }
    void set_music(int i, int32_t v) { if (in_range(i, kMusicVars)) music_[i] = v; }

    // The per-load clear: V0..V255 only. The declared half (256..511) and
    // the globals are untouched, as in the original's load.
    // [orig: WacScript_InitAndLoad memset(dword_C6B240, 0, 0x400) @0x4f95ee]
    void clear_numbered_mission_vars() {
        for (int i = 0; i < kNumberedMissionVars; ++i) mission_[i] = 0;
    }
    // Process-start state: every bank zero.
    void clear_all() { mission_.fill(0); global_.fill(0); music_.fill(0); }

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

private:
    static bool in_range(int i, int n) { return i >= 0 && i < n; }
    std::array<int32_t, kMissionVars> mission_{};
    std::array<int32_t, kGlobalVars> global_{};
    std::array<int32_t, kMusicVars> music_{};
};

} // namespace opennova::world
