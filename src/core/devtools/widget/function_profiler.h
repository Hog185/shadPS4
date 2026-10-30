// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

#include <imgui.h>

#include "core/devtools/hle_profiler.h"
#include "core/devtools/perf_report.h"
#include "core/devtools/thread_usage.h"

namespace Core::Devtools::Widget {

class FunctionProfiler {
public:
    void Draw();

    bool open = false;
    int top_n = 25;                // 25, 50 or 75
    bool measure_cpu = false;      // also time CPU use per call, which is slower
    bool exclude_waits = false;    // hide calls that block the thread (waits, sleeps, joins)
    bool detect_stutter = true;    // record slow frames and what ran during them
    bool pause_on_stutter = false; // freeze the window when a slow frame is found
    double stutter_threshold_ms = 50.0;

    // Lines about the build and the game for the top of a report, provided by the layer.
    std::string (*get_system_info)() = nullptr;

private:
    struct Row {
        const char* name;
        const char* library;
        u64 calls;
        u64 total_ns;
        u64 cpu_ns;
        u64 max_ns;
        double calls_per_sec;
        double share;         // percent of the time of all rows in the list
        bool is_wait = false; // the call blocks the thread instead of using the CPU
        double calls_per_frame = 0.0;
        double ms_per_frame = 0.0;
    };

    enum class Severity { Info, Good, Warning, Bad };
    struct Hint {
        Severity severity;
        std::string text;
    };

    struct FrameTotals {
        u64 total_ns = 0;
        u64 cpu_ns = 0;
    };

    // The most recent values of a measurement, drawn as a graph.
    template <size_t N>
    struct History {
        std::array<float, N> values{};
        size_t head = 0;
        size_t count = 0;

        void Push(float value) {
            values[head] = value;
            head = (head + 1) % N;
            count = std::min(count + 1, N);
        }
        void Clear() {
            head = 0;
            count = 0;
        }
        // Oldest first.
        std::vector<float> Ordered() const {
            std::vector<float> ordered;
            ordered.reserve(count);
            const size_t first = count < N ? 0 : head;
            for (size_t i = 0; i < count; i++) {
                ordered.push_back(values[(first + i) % N]);
            }
            return ordered;
        }
    };

    struct TopEntry {
        const char* name;
        double ms_per_frame;
        double percent;
    };

    void Refresh(double now);
    void ResetBaselines();
    void ClearHistory();
    void BuildRows(const HleProfiler::Snapshot& snapshot, std::vector<Row>& rows, double dt);
    void BuildHints();
    void OnGuestFrame();
    void UpdateFrameTotals(PerfReport::Spike* spike);

    // Rows after the wait and text filters, with shares and per-frame costs.
    std::vector<Row> VisibleFunctions(const ImGuiTextFilter* filter, size_t* hidden_waits) const;
    std::vector<Row> VisibleGpu(const ImGuiTextFilter* filter) const;
    std::vector<TopEntry> MakeTop(std::vector<std::pair<const char*, u64>> ranked) const;
    std::vector<TopEntry> TopCpu() const;
    std::vector<TopEntry> TopWaits() const;
    std::vector<TopEntry> TopGpu() const;
    double FrameBudgetMs() const;
    double FramesSinceReset() const;

    PerfReport::ReportData BuildReport() const;
    void CopyReport();
    void SaveReport();
    void SetStatus(std::string text);

    void DrawMenuBar();
    void DrawOverview();
    void DrawFunctions();
    void DrawThreads();
    void DrawGpu();
    void DrawStutterLog();
    void DrawTopTable(const char* id, const char* title, const std::vector<TopEntry>& entries);
    void DrawStatTable(const char* id, std::vector<Row>& rows, bool with_cpu,
                       const char* time_label, bool sort_by_cpu = false);

    std::vector<Row> function_rows;
    std::vector<Row> gpu_rows;
    std::vector<Row> queue_rows;
    std::vector<ThreadUsage::ThreadInfo> thread_rows;
    std::vector<Hint> hints;

    ImGuiTextFilter function_filter;
    ImGuiTextFilter gpu_filter;
    ImGuiTextFilter thread_filter;

    ThreadUsage::ThreadSampler sampler;
    std::unordered_map<const HleProfiler::FunctionStats*, u64> prev_calls;
    u64 function_registered = 0;
    double elapsed_seconds = 0.0;
    double last_refresh = -1.0;
    bool paused = false;
    bool was_paused = false;
    bool was_open = false;

    // Slow frame detection, one sample per guest frame.
    static constexpr size_t MAX_STUTTERS = 50;
    History<240> frame_ms_history;
    History<120> gpu_busy_history;
    History<120> hot_thread_history;
    PerfReport::FrameStats frame_stats;
    std::deque<PerfReport::Spike> stutters;
    std::unordered_map<const HleProfiler::FunctionStats*, FrameTotals> frame_prev;
    int last_frame_num = -1;
    int reset_frame_num = 0;
    double frames_since_reset = 1.0;

    std::string status_text;
    double status_until = 0.0;

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
