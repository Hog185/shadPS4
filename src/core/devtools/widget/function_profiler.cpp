// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <thread>

#include <imgui.h>

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
    ColTotal,
    ColCpu,
    ColWait,
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

double AverageNs(const auto& row) {
    return row.calls ? static_cast<double>(row.total_ns) / static_cast<double>(row.calls) : 0.0;
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

} // namespace

void FunctionProfiler::ResetBaselines() {
    prev_calls.clear();
    sampler.Reset();
    last_refresh = -1.0;
    prev_queue_ns = 0;
    prev_flips = static_cast<int>(DebugState.GetFrameNum());
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
                      : 0.0});
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
    for (const auto& thread : thread_rows) {
        process_cpu_percent += thread.cpu_percent;
    }

    // Guest frames are counted when the game flips.
    const int flips = static_cast<int>(DebugState.GetFrameNum());
    const int new_flips = flips - prev_flips;
    prev_flips = flips;
    fps = dt > 0.0 && new_flips > 0 ? new_flips / dt : 0.0;

    u64 queue_ns = 0;
    for (const auto& row : queue_rows) {
        queue_ns += row.total_ns;
    }
    const u64 new_queue_ns = queue_ns >= prev_queue_ns ? queue_ns - prev_queue_ns : queue_ns;
    prev_queue_ns = queue_ns;
    gpu_has_data = !queue_rows.empty() && dt > 0.0;
    gpu_busy_percent = dt > 0.0 ? static_cast<double>(new_queue_ns) / (dt * 1e9) * 100.0 : 0.0;
    gpu_ms_per_frame = new_flips > 0 ? static_cast<double>(new_queue_ns) / 1e6 / new_flips : 0.0;

    BuildHints();
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
        if (MenuItem("Measure CPU time (slower)", nullptr, &measure_cpu,
                     HleProfiler::CPU_TIME_SUPPORTED)) {
            HleProfiler::SetMeasureCpu(measure_cpu);
            HleProfiler::Reset();
            ResetBaselines();
        }
        if (MenuItem("Reset counters")) {
            HleProfiler::Reset();
            ResetBaselines();
        }
        EndMenu();
    }
    EndMenuBar();
}

void FunctionProfiler::DrawStatTable(const char* id, std::vector<Row>& rows, bool with_cpu,
                                     const char* time_label) {
    const size_t shown = std::min(rows.size(), static_cast<size_t>(top_n));
    const int columns = with_cpu ? 11 : 9;
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
    TableSetupColumn(time_label, desc | ImGuiTableColumnFlags_DefaultSort, 0.0f, ColTotal);
    if (with_cpu) {
        TableSetupColumn("CPU (ms)", desc, 0.0f, ColCpu);
        TableSetupColumn("Wait (ms)", desc, 0.0f, ColWait);
    }
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
            case ColTotal:
            case ColShare:
                return cmp(a.total_ns, b.total_ns);
            case ColCpu:
                return cmp(a.cpu_ns, b.cpu_ns);
            case ColWait:
                return cmp(WaitNs(a), WaitNs(b));
            case ColAverage:
                return cmp(AverageNs(a), AverageNs(b));
            case ColMax:
                return cmp(a.max_ns, b.max_ns);
            default:
                return false;
            }
        });
    }

    for (size_t i = 0; i < shown; i++) {
        const Row& row = rows[i];
        TableNextRow();
        TableNextColumn();
        Text("%zu", i + 1);
        TableNextColumn();
        TextUnformatted(row.name);
        TableNextColumn();
        TextUnformatted(row.library);
        TableNextColumn();
        Text("%llu", static_cast<unsigned long long>(row.calls));
        TableNextColumn();
        Text("%.0f", row.calls_per_sec);
        TableNextColumn();
        Text("%.3f", static_cast<double>(row.total_ns) / 1e6);
        if (with_cpu) {
            TableNextColumn();
            Text("%.3f", static_cast<double>(row.cpu_ns) / 1e6);
            TableNextColumn();
            Text("%.3f", static_cast<double>(WaitNs(row)) / 1e6);
        }
        TableNextColumn();
        Text("%.2f", AverageNs(row) / 1e3);
        TableNextColumn();
        Text("%.2f", static_cast<double>(row.max_ns) / 1e3);
        TableNextColumn();
        Text("%.1f%%", row.share);
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
    Separator();
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    Text("Process CPU: %.0f%% of %u%% available (%u logical cores)", process_cpu_percent,
         cores * 100, cores);
}

void FunctionProfiler::DrawFunctions() {
    const size_t shown = std::min(function_rows.size(), static_cast<size_t>(top_n));
    Text("Showing %zu of %zu active functions (%zu registered) | running for %.1fs", shown,
         function_rows.size(), static_cast<size_t>(function_registered), elapsed_seconds);
    DrawStatTable(measure_cpu ? "FunctionsCpu" : "Functions", function_rows, measure_cpu,
                  "Wall (ms)");
    TextDisabled("HLE library calls and timed host functions. Wall time includes waiting;\n"
                 "turn on 'Measure CPU time' to split it into CPU and Wait. Game code is native.");
}

void FunctionProfiler::DrawThreads() {
    if (!ThreadUsage::IsSupported()) {
        TextWrapped("Per-thread CPU usage is not available on this platform.");
        return;
    }
    const size_t shown = std::min(thread_rows.size(), static_cast<size_t>(top_n));
    Text("Showing %zu of %zu threads | 100%% is one core fully busy", shown, thread_rows.size());
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
                thread_rows.begin(), thread_rows.end(),
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
            const auto& thread = thread_rows[i];
            TableNextRow();
            TableNextColumn();
            Text("%zu", i + 1);
            TableNextColumn();
            TextUnformatted(thread.name.c_str());
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

    const size_t shown = std::min(gpu_rows.size(), static_cast<size_t>(top_n));
    Text("Draws and dispatches: showing %zu of %zu", shown, gpu_rows.size());
    DrawStatTable("GpuScopes", gpu_rows, false, "GPU (ms)");

    const u64 dropped = HleProfiler::g_state.gpu_dropped_scopes.load(std::memory_order_relaxed);
    if (dropped > 0) {
        TextColored(SeverityColor(2),
                    "%llu draws/dispatches were not timed (too many in one command buffer).",
                    static_cast<unsigned long long>(dropped));
    }
    TextDisabled(
        "GPU time between timestamps, so work that overlaps on the GPU is shared between\n"
        "neighbours. Command buffer time not covered by draws is copies, barriers and uploads.");
}

void FunctionProfiler::Draw() {
    // Counters only run while the window is open.
    HleProfiler::SetEnabled(open);
    if (!open) {
        return;
    }
    HleProfiler::SetMeasureCpu(measure_cpu);

    SetNextWindowSize({880.0f, 660.0f}, ImGuiCond_FirstUseEver);
    if (!Begin("Performance Profiler", &open, ImGuiWindowFlags_MenuBar)) {
        End();
        return;
    }
    DrawMenuBar();

    const double now = GetTime();
    if (was_paused && !paused) {
        ResetBaselines();
    }
    was_paused = paused;
    if (!paused && (last_refresh < 0.0 || now - last_refresh >= REFRESH_INTERVAL)) {
        Refresh(now);
    }
    if (paused) {
        TextColored(SeverityColor(2), "PAUSED");
        SameLine();
    }
    Text("Top %d lists", top_n);

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
