// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"

namespace Core::Devtools::ThreadUsage {

struct ThreadInfo {
    u64 id;
    std::string name;
    double cpu_percent;    // 100 means one core fully busy
    double user_percent;   // part of cpu_percent spent in user mode
    double kernel_percent; // part of cpu_percent spent in the kernel (syscalls, page faults)
    double total_cpu_seconds;
};

// Whether per-thread CPU times can be read on this platform.
[[nodiscard]] bool IsSupported();

class ThreadSampler {
public:
    // CPU usage of every thread of this process since the previous call. The first call after
    // construction or Reset returns an empty list because there is nothing to compare against.
    [[nodiscard]] std::vector<ThreadInfo> Sample();

    void Reset();

private:
    struct Previous {
        u64 user_ns;
        u64 kernel_ns;
    };
    std::unordered_map<u64, Previous> previous;
    std::chrono::steady_clock::time_point last_sample{};
    bool has_last_sample{};
};

} // namespace Core::Devtools::ThreadUsage
