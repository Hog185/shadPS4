// SPDX-License-Identifier: GPL-2.0-or-later
//
// ue4_oodle_hook: logs the plaintext packets an Unreal Engine 4 game hands to its Oodle packet compressor,
// so that the traffic of a client talking to a custom server can be read (it is Oodle compressed on the wire).
//
// Off unless SHADPS4_UE4OODLE_LOG names a file. The game's code is patched at the entry of
// OodleHandlerComponent::Outgoing (guest address 0x22ca5e0 in Fortnite 7.40, CUSA07022) once that code is
// loaded; the 17 bytes there must match the expected prologue or nothing is changed. Linux only.
//
// Output: one JSON object per line, in the same shape as the game server's capture of its own traffic, with
// "dir":"in" meaning client -> server:
//   {"bits":N,"dir":"in","hex":"...","len":BYTES,"peer":"client","src":"oodle-outgoing","t":MS_SINCE_EPOCH}
// "bits" is the packet's length in bits (it is not byte aligned); the last byte of "hex" is padded.
//
// Environment:
//   SHADPS4_UE4OODLE_LOG    file to append to; unset = feature off
//   SHADPS4_UE4OODLE_ADDR   hex guest address of Outgoing (default 0x22ca5e0)
#pragma once

namespace Core::Ue4OodleHook {

// Starts a thread that installs the hook when the game's code is in memory. Safe to call more than once.
void Start();

} // namespace Core::Ue4OodleHook
