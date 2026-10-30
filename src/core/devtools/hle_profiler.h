// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <vector>

#include "common/types.h"

namespace Core::Devtools::HleProfiler {

// Which list a function belongs to.
enum class Category : u8 {
    Cpu,      // HLE functions and PROFILE_SCOPE host functions
    Gpu,      // timed GPU work (draws, dispatches)
    GpuQueue, // GPU time of whole command buffers, one entry per scheduler
};
inline constexpr size_t NUM_CATEGORIES = 3;

// Counters for one function, stored in its HostCallWrapperImpl or PROFILE_SCOPE site.
struct FunctionStats {
    std::atomic<u64> calls{0};
    std::atomic<u64> total_ns{0};
    std::atomic<u64> max_ns{0};
    std::atomic<u64> cpu_ns{0}; // thread CPU time spent inside the calls, only if measured
    // Registry list node, filled in by Register.
    const char* name = nullptr;
    const char* library = nullptr;
    FunctionStats* next = nullptr;
    std::atomic<bool> registered{false};
};

struct State {
    std::atomic<bool> enabled{false};
    std::atomic<bool> measure_cpu{false};
    std::array<std::atomic<FunctionStats*>, NUM_CATEGORIES> head{};
    std::array<std::atomic<u64>, NUM_CATEGORIES> count{};
    std::atomic<s64> start_ns{0};
    std::atomic<u8> gpu_status{0};              // 0 unknown, 1 timing, 2 unsupported
    std::atomic<u64> gpu_dropped_scopes{0};     // scopes that did not fit in the query budget
    std::atomic<const char*> gpu_name{nullptr}; // set once by the first GpuProfiler
};

// Header-only so targets that compile HLE libraries without the devtools don't need to link it.
inline State g_state;

// True only while the profiler window is open.
[[nodiscard]] inline bool IsEnabled() {
    return g_state.enabled.load(std::memory_order_relaxed);
}

// CPU time per call needs a cheap per-thread clock, which only some platforms have.
#if defined(__linux__) || defined(__APPLE__)
inline constexpr bool CPU_TIME_SUPPORTED = true;
#else
inline constexpr bool CPU_TIME_SUPPORTED = false;
#endif

[[nodiscard]] inline bool MeasureCpu() {
    return g_state.measure_cpu.load(std::memory_order_relaxed);
}

inline void SetMeasureCpu(bool measure) {
    g_state.measure_cpu.store(measure && CPU_TIME_SUPPORTED, std::memory_order_relaxed);
}

// CPU time consumed by the calling thread, or -1 if unavailable.
inline s64 ThreadCpuNs() {
#if defined(__linux__) || defined(__APPLE__)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
        return -1;
    }
    return static_cast<s64>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#else
    return -1;
#endif
}

inline s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Adds one timed call to the counters.
inline void RecordSample(FunctionStats& stats, u64 ns) {
    stats.calls.fetch_add(1, std::memory_order_relaxed);
    stats.total_ns.fetch_add(ns, std::memory_order_relaxed);
    u64 prev = stats.max_ns.load(std::memory_order_relaxed);
    while (ns > prev && !stats.max_ns.compare_exchange_weak(prev, ns, std::memory_order_relaxed)) {
    }
}

// Adds a function to a list; the first name registered for a stats object wins.
inline void Register(FunctionStats* stats, const char* name, const char* library,
                     Category category = Category::Cpu) {
    if (stats->registered.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    const auto index = static_cast<size_t>(category);
    stats->name = name;
    stats->library = library;
    stats->next = g_state.head[index].load(std::memory_order_relaxed);
    while (!g_state.head[index].compare_exchange_weak(stats->next, stats, std::memory_order_release,
                                                      std::memory_order_relaxed)) {
    }
    g_state.count[index].fetch_add(1, std::memory_order_relaxed);
}

// Clears all counters and restarts the elapsed timer.
inline void Reset() {
    for (const auto& head : g_state.head) {
        for (auto* fn = head.load(std::memory_order_acquire); fn; fn = fn->next) {
            fn->calls.store(0, std::memory_order_relaxed);
            fn->total_ns.store(0, std::memory_order_relaxed);
            fn->max_ns.store(0, std::memory_order_relaxed);
            fn->cpu_ns.store(0, std::memory_order_relaxed);
        }
    }
    g_state.gpu_dropped_scopes.store(0, std::memory_order_relaxed);
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
    u64 cpu_ns;
    u64 max_ns;
};

struct Snapshot {
    std::vector<Entry> entries; // called at least once since the last reset
    u64 registered_count{};
    double elapsed_seconds{};
};

// Only needed by the devtools window, defined in hle_profiler.cpp.
[[nodiscard]] Snapshot TakeSnapshot(Category category = Category::Cpu);

class ScopedCall {
    using Clock = std::chrono::steady_clock;

public:
    explicit ScopedCall(FunctionStats& stats_) : stats{stats_} {
        if (IsEnabled()) {
            active = true;
            if (MeasureCpu()) {
                cpu_start = ThreadCpuNs();
            }
            start = Clock::now();
        }
    }

    ~ScopedCall() {
        if (!active) {
            return;
        }
        const auto ns = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
        if (cpu_start >= 0) {
            const s64 cpu_end = ThreadCpuNs();
            if (cpu_end >= cpu_start) {
                stats.cpu_ns.fetch_add(static_cast<u64>(cpu_end - cpu_start),
                                       std::memory_order_relaxed);
            }
        }
        RecordSample(stats, ns);
    }

    ScopedCall(const ScopedCall&) = delete;
    ScopedCall& operator=(const ScopedCall&) = delete;

private:
    FunctionStats& stats;
    Clock::time_point start{};
    s64 cpu_start = -1;
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
