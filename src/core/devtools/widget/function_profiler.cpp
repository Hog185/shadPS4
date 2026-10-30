// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

#include <fmt/format.h>
#include <imgui.h>

#include "common/path_util.h"
#include "core/debug_state.h"
#include "function_profiler.h"

using namespace ImGui;

namespace Core::Devtools::Widget {

namespace {

enum Column : ImGuiID {
    ColName = 1, // ID 0 belongs to the "#" column
    ColLibrary,
    ColCalls,
    ColCallsPerSec,
    ColCallsPerFrame,
    ColTotal,
    ColCpu,
    ColWait,
    ColMsPerFrame,
    ColAverage,
    ColMax,
    ColShare,
    ColThreadId,
    ColCpuPercent,
    ColUserPercent,
    ColKernelPercent,
    ColCpuSeconds,
};

constexpr double REFRESH_INTERVAL = 0.5; // seconds between list updates
constexpr double SATURATED_PERCENT = 90.0;
constexpr double HIGH_PERCENT = 70.0;
constexpr size_t TOP_COSTS = 5;
constexpr size_t SHOWN_STUTTERS = 10;
constexpr u64 MIN_CULPRIT_NS = 1'000'000;

double AverageNs(const auto& row) {
    return row.calls ? static_cast<double>(row.total_ns) / static_cast<double>(row.calls) : 0.0;
}

// Calls that block the thread instead of using the CPU, found by name.
bool IsWaitOrSleep(std::string_view name) {
    static constexpr std::array<std::string_view, 4> BLOCKING = {"wait", "sleep", "join", "delay"};
    // Names containing a blocking word that return right away.
    static constexpr std::array<std::string_view, 5> NOT_BLOCKING = {
        "trywait", "insertwait", "abortwait", "waittimeout", "eventwaiters"};
    std::string lower(name);
    std::ranges::transform(lower, lower.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const auto contains = [&](std::string_view word) {
        return lower.find(word) != std::string::npos;
    };
    if (std::ranges::any_of(NOT_BLOCKING, contains)) {
        return false;
    }
    return std::ranges::any_of(BLOCKING, contains);
}

u64 WaitNs(const auto& row) {
    return row.total_ns - std::min(row.total_ns, row.cpu_ns);
}

ImVec4 SeverityColor(int severity) {
    switch (severity) {
    case 1:
        return {0.45f, 0.9f, 0.45f, 1.0f};
    case 2:
        return {1.0f, 0.8f, 0.3f, 1.0f};
    case 3:
        return {1.0f, 0.45f, 0.45f, 1.0f};
    default:
        return GetStyleColorVec4(ImGuiCol_Text);
    }
}

// How much of a frame a cost takes: 3 for at least half of it, 2 for at least a quarter.
int CostSeverity(double ms, double frame_budget_ms) {
    if (ms >= frame_budget_ms * 0.5) {
        return 3;
    }
    return ms >= frame_budget_ms * 0.25 ? 2 : 0;
}

// A single slow call: 3 if it alone takes a whole frame, 2 for a quarter of one.
int SpikeSeverity(double ms, double frame_budget_ms) {
    if (ms >= frame_budget_ms) {
        return 3;
    }
    return ms >= frame_budget_ms * 0.25 ? 2 : 0;
}

std::string LocalTime(const char* format) {
    const auto now_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif
    std::ostringstream stream;
    stream << std::put_time(&local_tm, format);
    return stream.str();
}

std::string FormatCulprits(const std::vector<PerfReport::Culprit>& culprits) {
    std::string text;
    for (const auto& culprit : culprits) {
        text += fmt::format("{}{} {:.1f} ms", text.empty() ? "" : "; ", culprit.name, culprit.ms);
    }
    return text;
}

} // namespace

void FunctionProfiler::ResetBaselines() {
    prev_calls.clear();
    sampler.Reset();
    last_refresh = -1.0;
    prev_queue_ns = 0;
    prev_flips = static_cast<int>(DebugState.GetFrameNum());
}

void FunctionProfiler::ClearHistory() {
    frame_ms_history.Clear();
    gpu_busy_history.Clear();
    hot_thread_history.Clear();
    stutters.clear();
    frame_prev.clear();
    frame_stats = {};
    last_frame_num = -1;
    reset_frame_num = static_cast<int>(DebugState.GetFrameNum());
    frames_since_reset = 1.0;
}

double FunctionProfiler::FrameBudgetMs() const {
    return frame_stats.avg_ms > 1.0 ? frame_stats.avg_ms : 16.7;
}

double FunctionProfiler::FramesSinceReset() const {
    return frames_since_reset;
}

void FunctionProfiler::SetStatus(std::string text) {
    status_text = std::move(text);
    status_until = GetTime() + 8.0;
}

void FunctionProfiler::BuildRows(const HleProfiler::Snapshot& snapshot, std::vector<Row>& rows,
                                 double dt) {
    u64 total_ns = 0;
    for (const auto& e : snapshot.entries) {
        total_ns += e.total_ns;
    }
    rows.clear();
    rows.reserve(snapshot.entries.size());
    for (const auto& e : snapshot.entries) {
        // Rate over the last refresh interval, so it shows current activity.
        double rate = 0.0;
        if (const auto it = prev_calls.find(e.id); it != prev_calls.end() && dt > 0.0) {
            rate = static_cast<double>(e.calls - std::min(e.calls, it->second)) / dt;
        }
        prev_calls[e.id] = e.calls;
        rows.push_back(
            {e.name, e.library, e.calls, e.total_ns, e.cpu_ns, e.max_ns, rate,
             total_ns ? 100.0 * static_cast<double>(e.total_ns) / static_cast<double>(total_ns)
                      : 0.0,
             IsWaitOrSleep(e.name)});
    }
}

void FunctionProfiler::Refresh(double now) {
    const double dt = last_refresh < 0.0 ? 0.0 : now - last_refresh;
    last_refresh = now;

    const auto functions = HleProfiler::TakeSnapshot(HleProfiler::Category::Cpu);
    BuildRows(functions, function_rows, dt);
    function_registered = functions.registered_count;
    elapsed_seconds = functions.elapsed_seconds;
    BuildRows(HleProfiler::TakeSnapshot(HleProfiler::Category::Gpu), gpu_rows, dt);
    BuildRows(HleProfiler::TakeSnapshot(HleProfiler::Category::GpuQueue), queue_rows, dt);

    thread_rows = sampler.Sample();
    process_cpu_percent = 0.0;
    double hottest_percent = 0.0;
    for (const auto& thread : thread_rows) {
        process_cpu_percent += thread.cpu_percent;
        hottest_percent = std::max(hottest_percent, thread.cpu_percent);
    }

    // Guest frames are counted when the game flips.
    const int flips = static_cast<int>(DebugState.GetFrameNum());
    const int new_flips = flips - prev_flips;
    prev_flips = flips;
    fps = dt > 0.0 && new_flips > 0 ? new_flips / dt : 0.0;
    frames_since_reset = std::max(1, flips - reset_frame_num);

    u64 queue_ns = 0;
    for (const auto& row : queue_rows) {
        queue_ns += row.total_ns;
    }
    const u64 new_queue_ns = queue_ns >= prev_queue_ns ? queue_ns - prev_queue_ns : queue_ns;
    prev_queue_ns = queue_ns;
    gpu_has_data = !queue_rows.empty() && dt > 0.0;
    gpu_busy_percent = dt > 0.0 ? static_cast<double>(new_queue_ns) / (dt * 1e9) * 100.0 : 0.0;
    gpu_ms_per_frame = new_flips > 0 ? static_cast<double>(new_queue_ns) / 1e6 / new_flips : 0.0;

    if (dt > 0.0) {
        if (gpu_has_data) {
            gpu_busy_history.Push(static_cast<float>(gpu_busy_percent));
        }
        if (!thread_rows.empty()) {
            hot_thread_history.Push(static_cast<float>(hottest_percent));
        }
    }
    frame_stats = PerfReport::ComputeFrameStats(frame_ms_history.Ordered());

    BuildHints();
}

void FunctionProfiler::UpdateFrameTotals(PerfReport::Spike* spike) {
    const auto cpu = HleProfiler::TakeSnapshot(HleProfiler::Category::Cpu);
    const auto gpu = HleProfiler::TakeSnapshot(HleProfiler::Category::Gpu);
    if (spike) {
        spike->at_seconds = cpu.elapsed_seconds;
    }
    // Waiting inside other calls is left out too when CPU time is being measured.
    const bool by_cpu = exclude_waits && measure_cpu;

    std::vector<PerfReport::Culprit> cpu_costs;
    std::vector<PerfReport::Culprit> gpu_costs;
    const auto visit = [&](const HleProfiler::Snapshot& snapshot,
                           std::vector<PerfReport::Culprit>* costs, bool is_cpu) {
        for (const auto& e : snapshot.entries) {
            auto& prev = frame_prev[e.id];
            if (costs && !(is_cpu && exclude_waits && IsWaitOrSleep(e.name))) {
                const bool use_cpu = is_cpu && by_cpu;
                const u64 now_ns = use_cpu ? e.cpu_ns : e.total_ns;
                const u64 prev_ns = use_cpu ? prev.cpu_ns : prev.total_ns;
                // Anything under a millisecond is not what made the frame slow.
                if (now_ns > prev_ns && now_ns - prev_ns >= MIN_CULPRIT_NS) {
                    costs->push_back({e.name, static_cast<double>(now_ns - prev_ns) / 1e6});
                }
            }
            prev = {e.total_ns, e.cpu_ns};
        }
    };
    visit(cpu, spike ? &cpu_costs : nullptr, true);
    visit(gpu, spike ? &gpu_costs : nullptr, false);

    if (spike) {
        const auto keep_top = [](std::vector<PerfReport::Culprit>& costs, size_t count) {
            std::ranges::sort(costs, std::greater{}, &PerfReport::Culprit::ms);
            if (costs.size() > count) {
                costs.resize(count);
            }
        };
        keep_top(cpu_costs, 3);
        keep_top(gpu_costs, 2);
        spike->cpu = std::move(cpu_costs);
        spike->gpu = std::move(gpu_costs);
    }
}

void FunctionProfiler::OnGuestFrame() {
    if (DebugState.IsGuestThreadsPaused()) {
        return;
    }
    const int frame = static_cast<int>(DebugState.GetFrameNum());
    if (frame == last_frame_num) {
        return;
    }
    const bool seeded = last_frame_num >= 0;
    last_frame_num = frame;

    const double frame_ms = static_cast<double>(DebugState.FrameDeltaTime) * 1000.0;
    if (seeded) {
        frame_ms_history.Push(static_cast<float>(frame_ms));
    }
    if (!detect_stutter) {
        return;
    }
    const bool stutter = seeded && frame_ms >= stutter_threshold_ms;
    PerfReport::Spike spike;
    UpdateFrameTotals(stutter ? &spike : nullptr);
    if (!stutter) {
        return;
    }
    spike.frame = frame;
    spike.frame_ms = frame_ms;
    stutters.push_front(std::move(spike));
    if (stutters.size() > MAX_STUTTERS) {
        stutters.pop_back();
    }
    if (pause_on_stutter) {
        paused = true;
    }
}

void FunctionProfiler::BuildHints() {
    hints.clear();
    char text[256];

    if (fps > 0.0) {
        std::snprintf(text, sizeof(text),
                      "Game is presenting %.1f frames per second (%.1f ms per frame).", fps,
                      1000.0 / fps);
        hints.push_back({Severity::Info, text});
    } else {
        hints.push_back({Severity::Info, "The game did not present a frame in the last interval."});
    }

    const auto gpu_status = HleProfiler::g_state.gpu_status.load(std::memory_order_relaxed);
    if (gpu_status == 2) {
        hints.push_back({Severity::Info, "GPU timing is not supported by this GPU or driver."});
    } else if (!gpu_has_data) {
        hints.push_back(
            {Severity::Info, "GPU timing: waiting for the first command buffers to finish."});
    } else {
        std::snprintf(text, sizeof(text), "GPU is busy %.0f%% of the time (%.1f ms per frame).",
                      gpu_busy_percent, gpu_ms_per_frame);
        hints.push_back({gpu_busy_percent >= SATURATED_PERCENT ? Severity::Bad
                         : gpu_busy_percent >= HIGH_PERCENT    ? Severity::Warning
                                                               : Severity::Good,
                         text});
    }

    const ThreadUsage::ThreadInfo* hottest = nullptr;
    for (const auto& thread : thread_rows) {
        if (!hottest || thread.cpu_percent > hottest->cpu_percent) {
            hottest = &thread;
        }
    }
    if (hottest) {
        std::snprintf(text, sizeof(text),
                      "Busiest thread: %s at %.0f%% of one core (%.0f%% user, %.0f%% kernel).",
                      hottest->name.c_str(), hottest->cpu_percent, hottest->user_percent,
                      hottest->kernel_percent);
        hints.push_back({hottest->cpu_percent >= SATURATED_PERCENT ? Severity::Bad
                         : hottest->cpu_percent >= HIGH_PERCENT    ? Severity::Warning
                                                                   : Severity::Good,
                         text});
        if (hottest->kernel_percent >= 30.0 &&
            hottest->kernel_percent * 2.0 >= hottest->cpu_percent) {
            hints.push_back({Severity::Warning,
                             "That thread spends most of its time in the kernel: look for frequent "
                             "syscalls, page faults or memory protection changes."});
        }
    }

    const Row* top_wait = nullptr;
    const Row* top_cpu = nullptr;
    if (measure_cpu) {
        for (const auto& row : function_rows) {
            if (!top_wait || WaitNs(row) > WaitNs(*top_wait)) {
                top_wait = &row;
            }
            if (!top_cpu || row.cpu_ns > top_cpu->cpu_ns) {
                top_cpu = &row;
            }
        }
    }
    const Row* top_gpu = nullptr;
    for (const auto& row : gpu_rows) {
        if (!top_gpu || row.total_ns > top_gpu->total_ns) {
            top_gpu = &row;
        }
    }

    // Verdict, from the numbers above.
    const bool gpu_saturated = gpu_has_data && gpu_busy_percent >= SATURATED_PERCENT;
    const bool cpu_saturated = hottest && hottest->cpu_percent >= SATURATED_PERCENT;
    if (gpu_saturated) {
        std::snprintf(text, sizeof(text),
                      "Likely GPU-bound: the GPU is busy almost all the time.%s%s",
                      top_gpu ? " Most expensive work: " : "", top_gpu ? top_gpu->name : "");
        hints.push_back({Severity::Bad, text});
    }
    if (cpu_saturated) {
        std::snprintf(text, sizeof(text), "Likely CPU-bound on thread '%s'.%s%s",
                      hottest->name.c_str(),
                      top_cpu ? " Most CPU time inside calls to: "
                              : " Turn on 'Measure CPU time' to see which calls use it.",
                      top_cpu ? top_cpu->name : "");
        hints.push_back({Severity::Bad, text});
    }
    if (!gpu_saturated && !cpu_saturated) {
        if (fps > 0.0) {
            std::snprintf(text, sizeof(text),
                          "Neither the GPU nor any single thread is saturated, so the game is "
                          "probably waiting (frame limiter, vsync or synchronization).%s%s",
                          top_wait ? " Longest waits: "
                                   : " Turn on 'Measure CPU time' to see which calls wait.",
                          top_wait ? top_wait->name : "");
            hints.push_back({Severity::Good, text});
        } else {
            std::snprintf(text, sizeof(text),
                          "No frames are being presented, so the game is loading, paused or "
                          "stalled.%s%s",
                          top_wait ? " Longest waits: "
                                   : " Turn on 'Measure CPU time' to see which calls wait.",
                          top_wait ? top_wait->name : "");
            hints.push_back({Severity::Info, text});
        }
    }
}

std::vector<FunctionProfiler::Row> FunctionProfiler::VisibleFunctions(const ImGuiTextFilter* filter,
                                                                      size_t* hidden_waits) const {
    // With CPU timing the share is of CPU time, which leaves out waiting inside any call.
    const bool by_cpu = exclude_waits && measure_cpu;
    std::vector<Row> visible;
    visible.reserve(function_rows.size());
    u64 total = 0;
    for (const auto& row : function_rows) {
        if (exclude_waits && row.is_wait) {
            if (hidden_waits) {
                ++*hidden_waits;
            }
            continue;
        }
        // Filter on name and library together so excluded terms apply to both.
        if (filter && filter->IsActive() &&
            !filter->PassFilter(fmt::format("{} {}", row.name, row.library).c_str())) {
            continue;
        }
        visible.push_back(row);
        total += by_cpu ? row.cpu_ns : row.total_ns;
    }
    for (auto& row : visible) {
        const double basis = static_cast<double>(by_cpu ? row.cpu_ns : row.total_ns);
        row.share = total ? 100.0 * basis / static_cast<double>(total) : 0.0;
        row.calls_per_frame = static_cast<double>(row.calls) / frames_since_reset;
        row.ms_per_frame = basis / 1e6 / frames_since_reset;
    }
    return visible;
}

std::vector<FunctionProfiler::Row> FunctionProfiler::VisibleGpu(
    const ImGuiTextFilter* filter) const {
    std::vector<Row> visible;
    visible.reserve(gpu_rows.size());
    u64 total = 0;
    for (const auto& row : gpu_rows) {
        if (filter && filter->IsActive() && !filter->PassFilter(row.name)) {
            continue;
        }
        visible.push_back(row);
        total += row.total_ns;
    }
    for (auto& row : visible) {
        row.share =
            total ? 100.0 * static_cast<double>(row.total_ns) / static_cast<double>(total) : 0.0;
        row.calls_per_frame = static_cast<double>(row.calls) / frames_since_reset;
        row.ms_per_frame = static_cast<double>(row.total_ns) / 1e6 / frames_since_reset;
    }
    return visible;
}

std::vector<FunctionProfiler::TopEntry> FunctionProfiler::MakeTop(
    std::vector<std::pair<const char*, u64>> ranked) const {
    u64 total = 0;
    for (const auto& entry : ranked) {
        total += entry.second;
    }
    std::ranges::sort(ranked, std::greater{}, &std::pair<const char*, u64>::second);
    if (ranked.size() > TOP_COSTS) {
        ranked.resize(TOP_COSTS);
    }
    std::vector<TopEntry> entries;
    for (const auto& [name, ns] : ranked) {
        entries.push_back(
            {name, static_cast<double>(ns) / 1e6 / frames_since_reset,
             total ? 100.0 * static_cast<double>(ns) / static_cast<double>(total) : 0.0});
    }
    return entries;
}

std::vector<FunctionProfiler::TopEntry> FunctionProfiler::TopCpu() const {
    std::vector<std::pair<const char*, u64>> ranked;
    for (const auto& row : function_rows) {
        const u64 ns = measure_cpu ? row.cpu_ns : row.total_ns;
        if (!row.is_wait && ns > 0) {
            ranked.emplace_back(row.name, ns);
        }
    }
    return MakeTop(std::move(ranked));
}

std::vector<FunctionProfiler::TopEntry> FunctionProfiler::TopWaits() const {
    std::vector<std::pair<const char*, u64>> ranked;
    for (const auto& row : function_rows) {
        // With CPU timing every call can wait, otherwise only calls named like waits do.
        const u64 ns = measure_cpu ? WaitNs(row) : (row.is_wait ? row.total_ns : 0);
        if (ns > 0) {
            ranked.emplace_back(row.name, ns);
        }
    }
    return MakeTop(std::move(ranked));
}

std::vector<FunctionProfiler::TopEntry> FunctionProfiler::TopGpu() const {
    std::vector<std::pair<const char*, u64>> ranked;
    for (const auto& row : gpu_rows) {
        ranked.emplace_back(row.name, row.total_ns);
    }
    return MakeTop(std::move(ranked));
}

PerfReport::ReportData FunctionProfiler::BuildReport() const {
    PerfReport::ReportData data;
    if (get_system_info) {
        data.system_info = get_system_info();
    }
    if (const char* gpu_name = HleProfiler::g_state.gpu_name.load(std::memory_order_relaxed)) {
        data.system_info += fmt::format("GPU: {}\n", gpu_name);
    }
    data.system_info +=
        fmt::format("CPU: {} logical cores\n", std::max(1u, std::thread::hardware_concurrency()));
    data.timestamp = LocalTime("%Y-%m-%d %H:%M:%S");
    data.settings = fmt::format("top {}, {}, {}", top_n,
                                exclude_waits ? "waits and sleeps excluded" : "waits included",
                                measure_cpu ? "CPU time measured" : "wall time only");
    data.profiled_seconds = elapsed_seconds;
    data.frames = static_cast<int>(frames_since_reset);
    data.frame_stats = frame_stats;
    data.spike_threshold_ms = stutter_threshold_ms;
    for (const auto& hint : hints) {
        data.hints.push_back(hint.text);
    }

    const auto convert = [](const Row& row, bool with_cpu) {
        return PerfReport::Row{row.name,
                               row.calls,
                               static_cast<double>(row.total_ns) / 1e6,
                               with_cpu ? static_cast<double>(row.cpu_ns) / 1e6 : -1.0,
                               row.ms_per_frame,
                               row.calls_per_frame,
                               static_cast<double>(row.max_ns) / 1e3,
                               row.share};
    };

    const bool by_cpu = exclude_waits && measure_cpu;
    auto functions = VisibleFunctions(nullptr, nullptr);
    std::ranges::sort(functions, std::greater{},
                      [&](const Row& row) { return by_cpu ? row.cpu_ns : row.total_ns; });
    auto gpu = VisibleGpu(nullptr);
    std::ranges::sort(gpu, std::greater{}, &Row::total_ns);
    auto threads = thread_rows;
    std::ranges::sort(threads, std::greater{}, &ThreadUsage::ThreadInfo::cpu_percent);

    const auto limit = static_cast<size_t>(top_n);
    for (size_t i = 0; i < std::min(limit, functions.size()); i++) {
        data.functions.push_back(convert(functions[i], measure_cpu));
    }
    for (size_t i = 0; i < std::min(limit, gpu.size()); i++) {
        data.gpu.push_back(convert(gpu[i], false));
    }
    for (size_t i = 0; i < std::min(limit, threads.size()); i++) {
        data.threads.push_back({threads[i].name, threads[i].cpu_percent, threads[i].user_percent,
                                threads[i].kernel_percent});
    }
    data.spikes.assign(stutters.begin(), stutters.end());
    return data;
}

void FunctionProfiler::CopyReport() {
    SetClipboardText(PerfReport::FormatReport(BuildReport()).c_str());
    SetStatus("Report copied to the clipboard.");
}

void FunctionProfiler::SaveReport() {
    const auto directory = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const auto path = directory / fmt::format("profiler_report_{}.txt", LocalTime("%Y%m%d_%H%M%S"));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream file{path, std::ios::binary};
    if (!file) {
        SetStatus(fmt::format("Could not write {}", Common::FS::PathToUTF8String(path)));
        return;
    }
    file << PerfReport::FormatReport(BuildReport());
    SetStatus(fmt::format("Report saved to {}", Common::FS::PathToUTF8String(path)));
}

void FunctionProfiler::DrawMenuBar() {
    if (!BeginMenuBar()) {
        return;
    }
    if (BeginMenu("Show top")) {
        for (const int n : {25, 50, 75}) {
            char label[16];
            std::snprintf(label, sizeof(label), "Top %d", n);
            if (MenuItem(label, nullptr, top_n == n)) {
                top_n = n;
            }
        }
        EndMenu();
    }
    if (BeginMenu("Profiler")) {
        MenuItem("Pause updates", nullptr, &paused);
        MenuItem("Exclude waits and sleeps", nullptr, &exclude_waits);
        if (IsItemHovered()) {
            SetTooltip("Hides calls that block the thread (wait, sleep, join, delay) from the\n"
                       "Functions list. With 'Measure CPU time' on, shares and the default sort\n"
                       "use CPU time, so waiting inside other calls is left out too.");
        }
        if (MenuItem("Measure CPU time (slower)", nullptr, &measure_cpu,
                     HleProfiler::CPU_TIME_SUPPORTED)) {
            HleProfiler::SetMeasureCpu(measure_cpu);
            HleProfiler::Reset();
            ClearHistory();
            ResetBaselines();
        }
        if (MenuItem("Reset counters")) {
            HleProfiler::Reset();
            ClearHistory();
            ResetBaselines();
        }
        EndMenu();
    }
    if (BeginMenu("Stutter")) {
        MenuItem("Detect slow frames", nullptr, &detect_stutter);
        MenuItem("Pause when one is found", nullptr, &pause_on_stutter, detect_stutter);
        if (BeginMenu("Slow frame threshold", detect_stutter)) {
            for (const double ms : {25.0, 33.0, 50.0, 100.0, 250.0}) {
                char label[24];
                std::snprintf(label, sizeof(label), "%.0f ms", ms);
                if (MenuItem(label, nullptr, stutter_threshold_ms == ms)) {
                    stutter_threshold_ms = ms;
                }
            }
            EndMenu();
        }
        if (MenuItem("Clear stutter log")) {
            stutters.clear();
        }
        EndMenu();
    }
    if (BeginMenu("Report")) {
        if (MenuItem("Copy to clipboard")) {
            CopyReport();
        }
        if (MenuItem("Save to the log folder")) {
            SaveReport();
        }
        EndMenu();
    }
    EndMenuBar();
}

void FunctionProfiler::DrawTopTable(const char* id, const char* title,
                                    const std::vector<TopEntry>& entries) {
    Text("%s", title);
    if (entries.empty()) {
        TextDisabled("Nothing recorded yet.");
        return;
    }
    if (!BeginTable(id, 3,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_SizingFixedFit)) {
        return;
    }
    TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    TableSetupColumn("ms/frame");
    TableSetupColumn("Share");
    TableHeadersRow();
    const double budget = FrameBudgetMs();
    for (const auto& entry : entries) {
        TableNextRow();
        TableNextColumn();
        TextUnformatted(entry.name);
        TableNextColumn();
        PushStyleColor(ImGuiCol_Text, SeverityColor(CostSeverity(entry.ms_per_frame, budget)));
        Text("%.3f", entry.ms_per_frame);
        PopStyleColor();
        TableNextColumn();
        Text("%.1f%%", entry.percent);
    }
    EndTable();
}

void FunctionProfiler::DrawStutterLog() {
    SeparatorText("Stutter log");
    if (!detect_stutter) {
        TextDisabled("Slow frame detection is off (Stutter menu).");
        return;
    }
    Text("Frames slower than %.0f ms: %zu", stutter_threshold_ms, stutters.size());
    SameLine();
    if (SmallButton("Clear##stutters")) {
        stutters.clear();
    }
    if (stutters.empty()) {
        TextDisabled("None yet. The slowest calls just before a slow frame ended are listed here.");
        return;
    }
    if (BeginTable("Stutters", 4,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                       ImGuiTableFlags_SizingFixedFit)) {
        TableSetupColumn("Frame");
        TableSetupColumn("At (s)");
        TableSetupColumn("Time (ms)");
        TableSetupColumn("Slowest work before it", ImGuiTableColumnFlags_WidthStretch);
        TableHeadersRow();
        const double budget = FrameBudgetMs();
        size_t shown = 0;
        for (const auto& stutter : stutters) {
            if (shown++ >= SHOWN_STUTTERS) {
                break;
            }
            TableNextRow();
            TableNextColumn();
            Text("%d", stutter.frame);
            TableNextColumn();
            Text("%.1f", stutter.at_seconds);
            TableNextColumn();
            PushStyleColor(ImGuiCol_Text, SeverityColor(SpikeSeverity(stutter.frame_ms, budget)));
            Text("%.1f", stutter.frame_ms);
            PopStyleColor();
            TableNextColumn();
            std::string work;
            if (!stutter.cpu.empty()) {
                work += "CPU: " + FormatCulprits(stutter.cpu);
            }
            if (!stutter.gpu.empty()) {
                work +=
                    std::string(work.empty() ? "" : " | ") + "GPU: " + FormatCulprits(stutter.gpu);
            }
            TextWrapped("%s", work.empty() ? "(nothing was timed)" : work.c_str());
        }
        EndTable();
    }
    if (stutters.size() > SHOWN_STUTTERS) {
        TextDisabled("Showing the newest %zu of %zu. The report lists them all.", SHOWN_STUTTERS,
                     stutters.size());
    }
}

void FunctionProfiler::DrawStatTable(const char* id, std::vector<Row>& rows, bool with_cpu,
                                     const char* time_label, bool sort_by_cpu) {
    const size_t shown = std::min(rows.size(), static_cast<size_t>(top_n));
    const int columns = with_cpu ? 13 : 11;
    if (!BeginTable(id, columns,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_SizingFixedFit,
                    {0.0f, -GetTextLineHeightWithSpacing() * 2.0f})) {
        return;
    }
    const auto desc = ImGuiTableColumnFlags_PreferSortDescending;
    TableSetupScrollFreeze(0, 1);
    TableSetupColumn("#", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed, 32.0f);
    TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColName);
    TableSetupColumn("Library", ImGuiTableColumnFlags_DefaultHide, 0.0f, ColLibrary);
    TableSetupColumn("Calls", desc, 0.0f, ColCalls);
    TableSetupColumn("Calls/s", desc, 0.0f, ColCallsPerSec);
    TableSetupColumn("Calls/frame", desc | ImGuiTableColumnFlags_DefaultHide, 0.0f,
                     ColCallsPerFrame);
    TableSetupColumn(time_label, sort_by_cpu ? desc : desc | ImGuiTableColumnFlags_DefaultSort,
                     0.0f, ColTotal);
    if (with_cpu) {
        TableSetupColumn("CPU (ms)", sort_by_cpu ? desc | ImGuiTableColumnFlags_DefaultSort : desc,
                         0.0f, ColCpu);
        TableSetupColumn("Wait (ms)", desc, 0.0f, ColWait);
    }
    TableSetupColumn("ms/frame", desc, 0.0f, ColMsPerFrame);
    TableSetupColumn("Avg (us)", desc, 0.0f, ColAverage);
    TableSetupColumn("Max (us)", desc, 0.0f, ColMax);
    TableSetupColumn("Share", desc, 0.0f, ColShare);
    TableHeadersRow();

    if (const ImGuiTableSortSpecs* specs = TableGetSortSpecs(); specs && specs->SpecsCount > 0) {
        const auto& spec = specs->Specs[0];
        const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
        std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
            const auto cmp = [&](auto x, auto y) { return asc ? x < y : x > y; };
            switch (spec.ColumnUserID) {
            case ColName:
                return asc ? std::string_view{a.name} < std::string_view{b.name}
                           : std::string_view{a.name} > std::string_view{b.name};
            case ColLibrary:
                return asc ? std::string_view{a.library} < std::string_view{b.library}
                           : std::string_view{a.library} > std::string_view{b.library};
            case ColCalls:
                return cmp(a.calls, b.calls);
            case ColCallsPerSec:
                return cmp(a.calls_per_sec, b.calls_per_sec);
            case ColCallsPerFrame:
                return cmp(a.calls_per_frame, b.calls_per_frame);
            case ColTotal:
                return cmp(a.total_ns, b.total_ns);
            case ColCpu:
                return cmp(a.cpu_ns, b.cpu_ns);
            case ColWait:
                return cmp(WaitNs(a), WaitNs(b));
            case ColMsPerFrame:
                return cmp(a.ms_per_frame, b.ms_per_frame);
            case ColAverage:
                return cmp(AverageNs(a), AverageNs(b));
            case ColMax:
                return cmp(a.max_ns, b.max_ns);
            case ColShare:
                return cmp(a.share, b.share);
            default:
                return false;
            }
        });
    }

    const double budget = FrameBudgetMs();
    for (size_t i = 0; i < shown; i++) {
        const Row& row = rows[i];
        PushID(static_cast<int>(i));
        TableNextRow();
        TableNextColumn();
        Text("%zu", i + 1);
        TableNextColumn();
        TextUnformatted(row.name);
        if (IsItemHovered()) {
            BeginTooltip();
            TextUnformatted(row.name);
            Separator();
            Text("Library: %s", row.library);
            Text("Calls: %llu (%.1f per frame, %.0f per second)",
                 static_cast<unsigned long long>(row.calls), row.calls_per_frame,
                 row.calls_per_sec);
            Text("Time: %.3f ms total, %.2f us average, %.2f us slowest",
                 static_cast<double>(row.total_ns) / 1e6, AverageNs(row) / 1e3,
                 static_cast<double>(row.max_ns) / 1e3);
            if (with_cpu) {
                Text("CPU %.3f ms, waiting %.3f ms", static_cast<double>(row.cpu_ns) / 1e6,
                     static_cast<double>(WaitNs(row)) / 1e6);
            }
            Text("%.3f ms per frame, %.1f%% of the list", row.ms_per_frame, row.share);
            TextDisabled("Right-click to copy.");
            EndTooltip();
        }
        if (BeginPopupContextItem("row_menu")) {
            if (MenuItem("Copy name")) {
                SetClipboardText(row.name);
            }
            if (MenuItem("Copy row")) {
                const std::string text = fmt::format(
                    "{}: {} calls, {:.3f} ms total, {:.3f} ms/frame, max {:.1f} us, {:.1f}%",
                    row.name, row.calls, static_cast<double>(row.total_ns) / 1e6, row.ms_per_frame,
                    static_cast<double>(row.max_ns) / 1e3, row.share);
                SetClipboardText(text.c_str());
            }
            EndPopup();
        }
        TableNextColumn();
        TextUnformatted(row.library);
        TableNextColumn();
        Text("%llu", static_cast<unsigned long long>(row.calls));
        TableNextColumn();
        Text("%.0f", row.calls_per_sec);
        TableNextColumn();
        Text("%.2f", row.calls_per_frame);
        TableNextColumn();
        Text("%.3f", static_cast<double>(row.total_ns) / 1e6);
        if (with_cpu) {
            TableNextColumn();
            Text("%.3f", static_cast<double>(row.cpu_ns) / 1e6);
            TableNextColumn();
            Text("%.3f", static_cast<double>(WaitNs(row)) / 1e6);
        }
        TableNextColumn();
        PushStyleColor(ImGuiCol_Text, SeverityColor(CostSeverity(row.ms_per_frame, budget)));
        Text("%.3f", row.ms_per_frame);
        PopStyleColor();
        TableNextColumn();
        Text("%.2f", AverageNs(row) / 1e3);
        TableNextColumn();
        const double max_ms = static_cast<double>(row.max_ns) / 1e6;
        PushStyleColor(ImGuiCol_Text, SeverityColor(SpikeSeverity(max_ms, budget)));
        Text("%.2f", max_ms * 1e3);
        PopStyleColor();
        TableNextColumn();
        Text("%.1f%%", row.share);
        PopID();
    }
    EndTable();
}

void FunctionProfiler::DrawOverview() {
    TextDisabled("Hints are rules of thumb based on the numbers in the other tabs.");
    Spacing();
    for (const auto& hint : hints) {
        PushStyleColor(ImGuiCol_Text, SeverityColor(static_cast<int>(hint.severity)));
        TextWrapped("%s", hint.text.c_str());
        PopStyleColor();
        Spacing();
    }

    SeparatorText("Frame time");
    const auto plot = [](const char* id, const auto& history, const char* overlay, float scale_max,
                         float height) {
        PlotLines(id, history.values.data(), static_cast<int>(history.count),
                  history.count < history.values.size() ? 0 : static_cast<int>(history.head),
                  overlay, 0.0f, scale_max, {0.0f, height});
    };
    char overlay[160];
    if (frame_stats.count > 0) {
        std::snprintf(overlay, sizeof(overlay), "avg %.1f ms | 1%% low %.0f FPS | worst %.0f ms",
                      frame_stats.avg_ms,
                      frame_stats.p99_ms > 0.0 ? 1000.0 / frame_stats.p99_ms : 0.0,
                      frame_stats.max_ms);
    } else {
        std::snprintf(overlay, sizeof(overlay), "waiting for frames");
    }
    const auto frame_scale =
        static_cast<float>(std::max(FrameBudgetMs() * 2.0, std::min(frame_stats.max_ms, 250.0)));
    plot("##frame_time", frame_ms_history, overlay, frame_scale, 70.0f);
    TextDisabled("Last %zu frames. Stutter threshold: %.0f ms.", frame_ms_history.count,
                 stutter_threshold_ms);

    if (gpu_busy_history.count > 0) {
        std::snprintf(overlay, sizeof(overlay), "GPU busy %.0f%%", gpu_busy_percent);
        plot("##gpu_busy", gpu_busy_history, overlay, 100.0f, 45.0f);
    }
    if (hot_thread_history.count > 0) {
        const float newest =
            hot_thread_history
                .values[(hot_thread_history.head + hot_thread_history.values.size() - 1) %
                        hot_thread_history.values.size()];
        std::snprintf(overlay, sizeof(overlay), "busiest thread %.0f%% of a core", newest);
        plot("##hot_thread", hot_thread_history, overlay, 100.0f, 45.0f);
    }
    TextDisabled("GPU and thread graphs: one point every %.1f s.", REFRESH_INTERVAL);

    SeparatorText("Biggest costs per frame");
    DrawTopTable("TopCpu", measure_cpu ? "CPU time" : "Time, waits and sleeps left out", TopCpu());
    Spacing();
    DrawTopTable("TopWaits", measure_cpu ? "Waiting" : "Waits and sleeps (found by name)",
                 TopWaits());
    Spacing();
    DrawTopTable("TopGpu", "GPU time in draws and dispatches", TopGpu());

    DrawStutterLog();

    Separator();
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    Text("Process CPU: %.0f%% of %u%% available (%u logical cores)", process_cpu_percent,
         cores * 100, cores);
    Text("Draw calls: %.0f per frame   Dispatches: %.0f per frame", DebugState.GetDrawCallsAvg(),
         DebugState.GetDispatchesAvg());
    if (const char* gpu_name = HleProfiler::g_state.gpu_name.load(std::memory_order_relaxed)) {
        Text("GPU: %s", gpu_name);
    }
}

void FunctionProfiler::DrawFunctions() {
    function_filter.Draw("Filter (inc,-exc)", 240.0f);
    size_t hidden = 0;
    auto rows = VisibleFunctions(&function_filter, &hidden);
    const size_t shown = std::min(rows.size(), static_cast<size_t>(top_n));
    Text("Showing %zu of %zu active functions (%zu registered) | running for %.1fs", shown,
         rows.size(), static_cast<size_t>(function_registered), elapsed_seconds);
    if (exclude_waits) {
        SameLine();
        TextDisabled("| %zu waits and sleeps hidden", hidden);
    }
    const char* table_id = measure_cpu ? (exclude_waits ? "FunctionsCpuNoWaits" : "FunctionsCpu")
                                       : (exclude_waits ? "FunctionsNoWaits" : "Functions");
    DrawStatTable(table_id, rows, measure_cpu, "Wall (ms)", exclude_waits && measure_cpu);
    TextDisabled("HLE library calls and timed host functions. Wall time includes waiting;\n"
                 "turn on 'Measure CPU time' to split it into CPU and Wait. Game code is native.");
}

void FunctionProfiler::DrawThreads() {
    if (!ThreadUsage::IsSupported()) {
        TextWrapped("Per-thread CPU usage is not available on this platform.");
        return;
    }
    thread_filter.Draw("Filter (inc,-exc)", 240.0f);
    std::vector<ThreadUsage::ThreadInfo> rows;
    for (const auto& thread : thread_rows) {
        if (!thread_filter.IsActive() || thread_filter.PassFilter(thread.name.c_str())) {
            rows.push_back(thread);
        }
    }
    const size_t shown = std::min(rows.size(), static_cast<size_t>(top_n));
    Text("Showing %zu of %zu threads | 100%% is one core fully busy", shown, rows.size());
    if (BeginTable("Threads", 7,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                       ImGuiTableFlags_SizingFixedFit,
                   {0.0f, -GetTextLineHeightWithSpacing() * 2.0f})) {
        const auto desc = ImGuiTableColumnFlags_PreferSortDescending;
        TableSetupScrollFreeze(0, 1);
        TableSetupColumn("#", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                         32.0f);
        TableSetupColumn("Thread", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColName);
        TableSetupColumn("ID", ImGuiTableColumnFlags_DefaultHide, 0.0f, ColThreadId);
        TableSetupColumn("CPU %", desc | ImGuiTableColumnFlags_DefaultSort, 0.0f, ColCpuPercent);
        TableSetupColumn("User %", desc, 0.0f, ColUserPercent);
        TableSetupColumn("Kernel %", desc, 0.0f, ColKernelPercent);
        TableSetupColumn("CPU time (s)", desc, 0.0f, ColCpuSeconds);
        TableHeadersRow();

        if (const ImGuiTableSortSpecs* specs = TableGetSortSpecs();
            specs && specs->SpecsCount > 0) {
            const auto& spec = specs->Specs[0];
            const bool asc = spec.SortDirection == ImGuiSortDirection_Ascending;
            std::stable_sort(
                rows.begin(), rows.end(),
                [&](const ThreadUsage::ThreadInfo& a, const ThreadUsage::ThreadInfo& b) {
                    const auto cmp = [&](auto x, auto y) { return asc ? x < y : x > y; };
                    switch (spec.ColumnUserID) {
                    case ColName:
                        return cmp(std::string_view{a.name}, std::string_view{b.name});
                    case ColThreadId:
                        return cmp(a.id, b.id);
                    case ColCpuPercent:
                        return cmp(a.cpu_percent, b.cpu_percent);
                    case ColUserPercent:
                        return cmp(a.user_percent, b.user_percent);
                    case ColKernelPercent:
                        return cmp(a.kernel_percent, b.kernel_percent);
                    case ColCpuSeconds:
                        return cmp(a.total_cpu_seconds, b.total_cpu_seconds);
                    default:
                        return false;
                    }
                });
        }

        for (size_t i = 0; i < shown; i++) {
            const auto& thread = rows[i];
            PushID(static_cast<int>(i));
            TableNextRow();
            TableNextColumn();
            Text("%zu", i + 1);
            TableNextColumn();
            TextUnformatted(thread.name.c_str());
            if (BeginPopupContextItem("thread_menu")) {
                if (MenuItem("Copy name")) {
                    SetClipboardText(thread.name.c_str());
                }
                EndPopup();
            }
            TableNextColumn();
            Text("%llu", static_cast<unsigned long long>(thread.id));
            TableNextColumn();
            PushStyleColor(ImGuiCol_Text, SeverityColor(thread.cpu_percent >= SATURATED_PERCENT ? 3
                                                        : thread.cpu_percent >= HIGH_PERCENT ? 2
                                                                                             : 0));
            Text("%.1f", thread.cpu_percent);
            PopStyleColor();
            TableNextColumn();
            Text("%.1f", thread.user_percent);
            TableNextColumn();
            Text("%.1f", thread.kernel_percent);
            TableNextColumn();
            Text("%.2f", thread.total_cpu_seconds);
            PopID();
        }
        EndTable();
    }
    TextDisabled("CPU use of every thread of the emulator, including guest threads.");
}

void FunctionProfiler::DrawGpu() {
    const auto gpu_status = HleProfiler::g_state.gpu_status.load(std::memory_order_relaxed);
    if (gpu_status == 2) {
        TextWrapped("GPU timing is not supported by this GPU or driver.");
        return;
    }
    if (!gpu_has_data) {
        TextWrapped("Waiting for the GPU to finish the first command buffers. Timing starts when "
                    "this window opens, and needs the Vulkan renderer to be running a game.");
        return;
    }

    Text("GPU busy %.0f%% of the time (%.2f ms per frame)", gpu_busy_percent, gpu_ms_per_frame);
    Text("Command buffers:");
    if (BeginTable("GpuQueues", 4,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                       ImGuiTableFlags_SizingFixedFit)) {
        TableSetupColumn("Scheduler", ImGuiTableColumnFlags_WidthStretch);
        TableSetupColumn("Submits");
        TableSetupColumn("GPU (ms)");
        TableSetupColumn("Avg (us)");
        TableHeadersRow();
        for (const auto& row : queue_rows) {
            TableNextRow();
            TableNextColumn();
            TextUnformatted(row.name);
            TableNextColumn();
            Text("%llu", static_cast<unsigned long long>(row.calls));
            TableNextColumn();
            Text("%.3f", static_cast<double>(row.total_ns) / 1e6);
            TableNextColumn();
            Text("%.2f", AverageNs(row) / 1e3);
        }
        EndTable();
    }

    gpu_filter.Draw("Filter (inc,-exc)", 240.0f);
    auto rows = VisibleGpu(&gpu_filter);
    const size_t shown = std::min(rows.size(), static_cast<size_t>(top_n));
    Text("Draws and dispatches: showing %zu of %zu", shown, rows.size());
    DrawStatTable("GpuScopes", rows, false, "GPU (ms)");

    const u64 dropped = HleProfiler::g_state.gpu_dropped_scopes.load(std::memory_order_relaxed);
    if (dropped > 0) {
        TextColored(SeverityColor(2),
                    "%llu draws/dispatches were not timed (too many in one command buffer).",
                    static_cast<unsigned long long>(dropped));
    }
    TextDisabled("GPU time between timestamps, so work that overlaps on the GPU is shared between\n"
                 "neighbours. Command buffer time not covered by draws is copies, barriers and "
                 "uploads.");
}

void FunctionProfiler::Draw() {
    // Counters only run while the window is open.
    HleProfiler::SetEnabled(open);
    if (!open) {
        was_open = false;
        return;
    }
    HleProfiler::SetMeasureCpu(measure_cpu);
    if (!was_open) {
        was_open = true;
        paused = false;
        ClearHistory();
        ResetBaselines();
    }

    // Data is collected even while the window is collapsed.
    const double now = GetTime();
    if (was_paused && !paused) {
        ResetBaselines();
    }
    was_paused = paused;
    if (!paused) {
        OnGuestFrame();
        if (last_refresh < 0.0 || now - last_refresh >= REFRESH_INTERVAL) {
            Refresh(now);
        }
    }

    SetNextWindowSize({880.0f, 660.0f}, ImGuiCond_FirstUseEver);
    if (!Begin("Performance Profiler", &open, ImGuiWindowFlags_MenuBar)) {
        End();
        return;
    }
    DrawMenuBar();

    if (paused) {
        TextColored(SeverityColor(2), "PAUSED");
        SameLine();
    }
    Text("Top %d lists", top_n);
    if (!status_text.empty() && now < status_until) {
        SameLine();
        TextColored(SeverityColor(1), "| %s", status_text.c_str());
    }

    if (BeginTabBar("PerformanceTabs")) {
        if (BeginTabItem("Overview")) {
            DrawOverview();
            EndTabItem();
        }
        if (BeginTabItem("Functions")) {
            DrawFunctions();
            EndTabItem();
        }
        if (BeginTabItem("Threads")) {
            DrawThreads();
            EndTabItem();
        }
        if (BeginTabItem("GPU")) {
            DrawGpu();
            EndTabItem();
        }
        EndTabBar();
    }
    End();
}

} // namespace Core::Devtools::Widget
