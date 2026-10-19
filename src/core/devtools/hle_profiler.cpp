// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "hle_profiler.h"

namespace Core::Devtools::HleProfiler {

Snapshot TakeSnapshot() {
    Snapshot snapshot;
    snapshot.registered_count = g_state.count.load(std::memory_order_relaxed);
    const s64 start_ns = g_state.start_ns.load(std::memory_order_relaxed);
    snapshot.elapsed_seconds = start_ns ? static_cast<double>(NowNs() - start_ns) / 1e9 : 0.0;
    for (auto* fn = g_state.head.load(std::memory_order_acquire); fn; fn = fn->next) {
        const u64 calls = fn->calls.load(std::memory_order_relaxed);
        if (calls == 0) {
            continue;
        }
        snapshot.entries.push_back({fn, fn->name, fn->library, calls,
                                    fn->total_ns.load(std::memory_order_relaxed),
                                    fn->max_ns.load(std::memory_order_relaxed)});
    }
    return snapshot;
}

} // namespace Core::Devtools::HleProfiler
