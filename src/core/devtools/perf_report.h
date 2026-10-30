// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <span>
#include <string>
#include <vector>

#include "common/types.h"

namespace Core::Devtools::PerfReport {

struct FrameStats {
    size_t count{};
    double avg_ms{};
    double min_ms{};
    double max_ms{};
    double p99_ms{}; // 99th percentile, so 1000 / p99_ms is the "1% low" frame rate
};

[[nodiscard]] FrameStats ComputeFrameStats(std::span<const float> frame_ms);

struct Row {
    std::string name;
    u64 calls{};
    double total_ms{};
    double cpu_ms{}; // negative when CPU time was not measured
    double ms_per_frame{};
    double calls_per_frame{};
    double max_us{};
    double share{};
};

struct ThreadRow {
    std::string name;
    double cpu_percent{};
    double user_percent{};
    double kernel_percent{};
};

struct Culprit {
    std::string name;
    double ms{};
};

// A frame that took longer than the stutter threshold, and what ran just before it finished.
struct Spike {
    int frame{};
    double frame_ms{};
    double at_seconds{}; // since the profiler was started
    std::vector<Culprit> cpu;
    std::vector<Culprit> gpu;
};

struct ReportData {
    std::string system_info; // lines about the build, game and GPU
    std::string timestamp;
    std::string settings; // e.g. "top 25, waits excluded"
    double profiled_seconds{};
    int frames{};
    FrameStats frame_stats;
    double spike_threshold_ms{};
    std::vector<std::string> hints;
    std::vector<Row> functions;
    std::vector<Row> gpu;
    std::vector<ThreadRow> threads;
    std::vector<Spike> spikes;
};

// Plain text, meant to be pasted into an issue or a pull request.
[[nodiscard]] std::string FormatReport(const ReportData& data);

} // namespace Core::Devtools::PerfReport
