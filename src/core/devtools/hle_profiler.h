// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <vector>

#include "common/types.h"

namespace Core::Devtools::HleProfiler {

// Counters for one function, stored in its HostCallWrapperImpl or PROFILE_SCOPE site.
struct FunctionStats {
    std::atomic<u64> calls{0};
    std::atomic<u64> total_ns{0};
    std::atomic<u64> max_ns{0};
};

extern std::atomic<bool> g_enabled;

// True only while the profiler window is open.
[[nodiscard]] inline bool IsEnabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

// Enabling clears all counters.
void SetEnabled(bool enabled);

// Adds a function to the list; the first name registered for a stats object wins.
void Register(FunctionStats* stats, const char* name, const char* library);

// Clears all counters and restarts the elapsed timer.
void Reset();

struct Entry {
    const FunctionStats* id;
    const char* name;
    const char* library;
    u64 calls;
    u64 total_ns;
    u64 max_ns;
};

struct Snapshot {
    std::vector<Entry> entries; // called at least once since the last reset
    u64 registered_count{};
    double elapsed_seconds{};
};

[[nodiscard]] Snapshot TakeSnapshot();

class ScopedCall {
    using Clock = std::chrono::steady_clock;

public:
    explicit ScopedCall(FunctionStats& stats_) : stats{stats_} {
        if (IsEnabled()) {
            active = true;
            start = Clock::now();
        }
    }

    ~ScopedCall() {
        if (!active) {
            return;
        }
        const auto ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        stats.calls.fetch_add(1, std::memory_order_relaxed);
        stats.total_ns.fetch_add(ns, std::memory_order_relaxed);
        u64 prev = stats.max_ns.load(std::memory_order_relaxed);
        while (ns > prev &&
               !stats.max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
        }
    }

    ScopedCall(const ScopedCall&) = delete;
    ScopedCall& operator=(const ScopedCall&) = delete;

private:
    FunctionStats& stats;
    Clock::time_point start{};
    bool active{};
};

} // namespace Core::Devtools::HleProfiler

// Times a host function; use once at the top of the function with a unique name.
#define PROFILE_SCOPE(display_name)                                                                \
    static ::Core::Devtools::HleProfiler::FunctionStats profile_scope_stats;                       \
    [[maybe_unused]] static const bool profile_scope_registered =                                  \
        (::Core::Devtools::HleProfiler::Register(&profile_scope_stats, display_name, "host"),      \
         true);                                                                                    \
    ::Core::Devtools::HleProfiler::ScopedCall profile_scope_call {                                 \
        profile_scope_stats                                                                        \
    }
