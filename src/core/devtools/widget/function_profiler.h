// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/devtools/hle_profiler.h"
#include "core/devtools/thread_usage.h"

namespace Core::Devtools::Widget {

class FunctionProfiler {
public:
    void Draw();

    bool open = false;
    int top_n = 25;           // 25, 50 or 75
    bool measure_cpu = false; // also time CPU use per call, which is slower

private:
    struct Row {
        const char* name;
        const char* library;
        u64 calls;
        u64 total_ns;
        u64 cpu_ns;
        u64 max_ns;
        double calls_per_sec;
        double share; // percent of the time of all rows in the list
    };

    enum class Severity { Info, Good, Warning, Bad };
    struct Hint {
        Severity severity;
        std::string text;
    };

    void Refresh(double now);
    void ResetBaselines();
    void BuildRows(const HleProfiler::Snapshot& snapshot, std::vector<Row>& rows, double dt);
    void BuildHints();

    void DrawMenuBar();
    void DrawOverview();
    void DrawFunctions();
    void DrawThreads();
    void DrawGpu();
    void DrawStatTable(const char* id, std::vector<Row>& rows, bool with_cpu,
                       const char* time_label);

    std::vector<Row> function_rows;
    std::vector<Row> gpu_rows;
    std::vector<Row> queue_rows;
    std::vector<ThreadUsage::ThreadInfo> thread_rows;
    std::vector<Hint> hints;

    ThreadUsage::ThreadSampler sampler;
    std::unordered_map<const HleProfiler::FunctionStats*, u64> prev_calls;
    u64 function_registered = 0;
    double elapsed_seconds = 0.0;
    double last_refresh = -1.0;
    bool paused = false;
    bool was_paused = false;

    // Overview numbers.
    double fps = 0.0;
    double gpu_busy_percent = 0.0;
    double gpu_ms_per_frame = 0.0;
    bool gpu_has_data = false;
    double process_cpu_percent = 0.0;
    u64 prev_queue_ns = 0;
    int prev_flips = 0;
};

} // namespace Core::Devtools::Widget
