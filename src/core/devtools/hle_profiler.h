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
    // Registry list node, filled in by Register.
    const char* name = nullptr;
    const char* library = nullptr;
    FunctionStats* next = nullptr;
    std::atomic<bool> registered{false};
};

struct State {
    std::atomic<bool> enabled{false};
    std::atomic<FunctionStats*> head{nullptr};
    std::atomic<u64> count{0};
    std::atomic<s64> start_ns{0};
};

// Header-only so targets that compile HLE libraries without the devtools don't need to link it.
inline State g_state;

// True only while the profiler window is open.
[[nodiscard]] inline bool IsEnabled() {
    return g_state.enabled.load(std::memory_order_relaxed);
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Adds a function to the list; the first name registered for a stats object wins.
inline void Register(FunctionStats* stats, const char* name, const char* library) {
    if (stats->registered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    stats->name = name;
    stats->library = library;
    stats->next = g_state.head.load(std::memory_order_relaxed);
    while (!g_state.head.compare_exchange_weak(stats->next, stats, std::memory_order_release,
                                               std::memory_order_relaxed)) {
    }
    g_state.count.fetch_add(1, std::memory_order_relaxed);
}

// Clears all counters and restarts the elapsed timer.
inline void Reset() {
    for (auto* fn = g_state.head.load(std::memory_order_acquire); fn; fn = fn->next) {
        fn->calls.store(0, std::memory_order_relaxed);
        fn->total_ns.store(0, std::memory_order_relaxed);
        fn->max_ns.store(0, std::memory_order_relaxed);
    }
    g_state.start_ns.store(NowNs(), std::memory_order_relaxed);
}

// Enabling clears all counters.
inline void SetEnabled(bool enabled) {
    if (IsEnabled() == enabled) {
        return;
    }
    if (enabled) {
        Reset();
    }
    g_state.enabled.store(enabled, std::memory_order_relaxed);
}

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

// Only needed by the devtools window, defined in hle_profiler.cpp.
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
