// The handler bodies of the WAC commands the host replicates over S2C 0x23
// (registry flags 0x18): text and wave with their targeted ptext/pwave/pconsol
// twins, the SSN hide/disable/hold family, teleSSN, SSNwave/SSNradio, SS2SSN,
// the marker/target effect and sound commands, flash/farflash, quake, music,
// text#/consol/consol#, fx2ssn, sound and face. The host VM runs them with
// operands resolved from its program; a joiner runs the same bodies with the
// operands the wire carried, which is why they take no Program.
// [orig: WacScript_ExecuteBytecode @0x4F58B0 call-convention switch
//  @0x4f5ef6; GameMode_DispatchRemoteCommand @0x4F81E0 switch @0x4f8438 —
//  both invoke the registry row's handler]
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <runtime/world/script_remote_command.h>

namespace opennova::world {
class World;
}

namespace opennova::wac {

// The name tables a Fx or SoundSet operand indexes: the 1-based compile-time
// handles the program interned. A peer without the program passes what its
// mounted catalogs hold.
struct RemoteCommandNames {
    const std::vector<std::string> *effects = nullptr;
    const std::vector<std::string> *sounds = nullptr;
};

// True for a registry row whose flags carry 0x18.
// [orig: byte_82D290[44 * idx] & 0x18 @0x4f5ca5 / @0x4f8429]
bool is_remote_command(int command_index);

// The index a replicated command travels under: the FIRST registry row that
// shares its handler (ptext -> text, pwave -> wave, pconsol -> consol).
// [orig: WacScript_ExecuteBytecode @0x4f5cb5..0x4f5cce]
int remote_command_wire_index(int command_index);

// Run the row's handler against the world with resolved operands, one per
// declared parameter; a missing operand reads as zero / empty, the client's
// zero-filled default. Returns the handler's value.
int32_t run_remote_command(world::World &world, int command_index,
                           const std::vector<world::ScriptRemoteArg> &args,
                           const RemoteCommandNames &names);

} // namespace opennova::wac
