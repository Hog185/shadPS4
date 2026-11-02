// SPDX-License-Identifier: GPL-2.0-or-later
//
// ue4_memdump: dump the guest's memory from inside the emulator so that tools/ue4_dump.py can read
// the Unreal Engine reflection data (classes, replicated properties, RPCs) of a running game.
//
// Off unless the environment variable SHADPS4_UE4DUMP_DIR names a directory. Then a background thread
// watches for the file <dir>/trigger; when it appears it is deleted and one dump is written to
// <dir>/ue4dump-<date>-<time>.bin.
//
// Environment (all optional except the first):
//   SHADPS4_UE4DUMP_DIR        directory for trigger and dumps; unset = feature off
//   SHADPS4_UE4DUMP_MIN/MAX    hex guest address range to dump (default 0x400000 - 0xFC00000000)
//   SHADPS4_UE4DUMP_MAX_BYTES  stop after this many bytes (default 8 GiB)
#pragma once

#include <string>

namespace Core::Ue4MemDump {

// Starts the trigger watcher if SHADPS4_UE4DUMP_DIR is set. Safe to call more than once.
void Start();

// Writes one dump now. Returns its path, or an empty string on failure.
std::string DumpNow();

} // namespace Core::Ue4MemDump
