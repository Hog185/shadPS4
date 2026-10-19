// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <mutex>
#include <unordered_set>

#include "hle_profiler.h"

namespace Core::Devtools::HleProfiler {

std::atomic<bool> g_enabled{false};

namespace {

struct Registered {
    FunctionStats* stats;
    const char* name;
    const char* library;
};

// Function-local statics avoid static initialization order problems.
struct Registry {
    std::mutex mutex;
    std::vector<Registered> functions;
    std::unordered_set<const FunctionStats*> known;
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

} // namespace

void Register(FunctionStats* stats, const char* name, const char* library) {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    if (registry.known.insert(stats).second) {
        registry.functions.push_back({stats, name, library});
    }
}

void Reset() {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    for (const auto& fn : registry.functions) {
        fn.stats->calls.store(0, std::memory_order_relaxed);
        fn.stats->total_ns.store(0, std::memory_order_relaxed);
        fn.stats->max_ns.store(0, std::memory_order_relaxed);
    }
    registry.start_time = std::chrono::steady_clock::now();
}

void SetEnabled(bool enabled) {
    if (g_enabled.load(std::memory_order_relaxed) == enabled) {
        return;
    }
    if (enabled) {
        Reset();
    }
    g_enabled.store(enabled, std::memory_order_relaxed);
}

Snapshot TakeSnapshot() {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    Snapshot snapshot;
    snapshot.registered_count = registry.functions.size();
    snapshot.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - registry.start_time)
            .count();
    for (const auto& fn : registry.functions) {
        const u64 calls = fn.stats->calls.load(std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        snapshot.entries.push_back({fn.stats, fn.name, fn.library, calls,
                                    fn.stats->total_ns.load(std::memory_order_relaxed),
                                    fn.stats->max_ns.load(std::memory_order_relaxed)});
    }
    return snapshot;
}

} // namespace Core::Devtools::HleProfiler
