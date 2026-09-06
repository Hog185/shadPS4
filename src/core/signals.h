// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <set>
#include <signal.h>
#include <string>
#include <vector>
#include "common/singleton.h"
#include "common/types.h"

#ifdef _WIN32
#define SIGSLEEP -1
#else
#define SIGSLEEP SIGVTALRM
#endif
namespace Core {

class Linker;

using AccessViolationHandler = bool (*)(void* context, void* fault_address);
using IllegalInstructionHandler = bool (*)(void* context);

#ifndef _WIN32
void SignalHandler(int sig, siginfo_t* info, void* raw_context);
#endif

// Best-effort native stack walker used to symbolicate crash addresses against loaded guest
// modules. RBP-chain based (SysV frame-pointer convention). Safe to call from within a signal
// handler: touches no emulator locks, only OS-level page queries and the Linker's already-
// populated module list.
class StackTracer {
public:
    struct Frame {
        VAddr return_addr{};
        bool has_module{};
        std::string module_name;
        u32 segment_index{};
        VAddr segment_offset{};
        bool has_symbol{};
        std::string symbol_name;
        VAddr symbol_offset{};
    };

    /// Must be called once the Linker owning loaded modules exists. Before that, frames just
    /// report as unmapped instead of failing.
    static void RegisterLinker(Linker* linker);

    /// context is a platform ucontext_t*/EXCEPTION_POINTERS*; null captures from the call site.
    static std::vector<VAddr> Capture(void* context, u32 max_frames = 64);

    static std::vector<Frame> Resolve(const std::vector<VAddr>& return_addrs);

    /// Renders in the "offset $SEG|OFFSET  module:symbol+$delta" crash-log format.
    static std::string Format(const std::vector<Frame>& frames);

    /// Capture + Resolve + Format in one call.
    static std::string Dump(void* context = nullptr, u32 max_frames = 64);
};

/// Receives OS signals and dispatches to the appropriate handlers.
class SignalDispatch {
public:
    SignalDispatch();
    ~SignalDispatch();

    void RemoveHandlers();

    /// Registers a handler for memory access violation signals.
    void RegisterAccessViolationHandler(const AccessViolationHandler& handler, u32 priority) {
        access_violation_handlers.emplace(handler, priority);
    }

    /// Registers a handler for illegal instruction signals.
    void RegisterIllegalInstructionHandler(const IllegalInstructionHandler& handler, u32 priority) {
        illegal_instruction_handlers.emplace(handler, priority);
    }

    /// Dispatches an access violation signal, returning whether it was successfully handled.
    bool DispatchAccessViolation(void* context, void* fault_address) const;

    /// Dispatches an illegal instruction signal, returning whether it was successfully handled.
    bool DispatchIllegalInstruction(void* context) const;

private:
    template <typename T>
    struct HandlerEntry {
        T handler;
        u32 priority;

        std::strong_ordering operator<=>(const HandlerEntry& right) const {
            return priority <=> right.priority;
        }
    };
    std::set<HandlerEntry<AccessViolationHandler>> access_violation_handlers;
    std::set<HandlerEntry<IllegalInstructionHandler>> illegal_instruction_handlers;

#ifdef _WIN32
    void* handle{};
#endif
};

using Signals = Common::Singleton<SignalDispatch>;

} // namespace Core