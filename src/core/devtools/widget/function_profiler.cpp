// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdio>
#include <string_view>

#include <imgui.h>

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
    ColAverage,
    ColMax,
    ColShare,
};

constexpr double REFRESH_INTERVAL = 0.5; // seconds between list updates

double AverageNs(const auto& row) {
    return row.calls ? static_cast<double>(row.total_ns) / static_cast<double>(row.calls) : 0.0;
}

} // namespace

void FunctionProfiler::Refresh(double now) {
    const auto snapshot = HleProfiler::TakeSnapshot();
    const double dt = last_refresh < 0.0 ? 0.0 : now - last_refresh;
    last_refresh = now;

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
            {e.name, e.library, e.calls, e.total_ns, e.max_ns, rate,
             total_ns ? 100.0 * static_cast<double>(e.total_ns) / static_cast<double>(total_ns)
                      : 0.0});
    }
    registered_count = snapshot.registered_count;
    elapsed_seconds = snapshot.elapsed_seconds;
}

void FunctionProfiler::SortRows() {
    const ImGuiTableSortSpecs* specs = TableGetSortSpecs();
    if (!specs || specs->SpecsCount == 0) {
        return;
    }
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
        case ColAverage:
            return cmp(AverageNs(a), AverageNs(b));
        case ColMax:
            return cmp(a.max_ns, b.max_ns);
        default:
            return false;
        }
    });
}

void FunctionProfiler::Draw() {
    // Counters only run while the window is open.
    HleProfiler::SetEnabled(open);
    if (!open) {
        return;
    }

    SetNextWindowSize({820.0f, 640.0f}, ImGuiCond_FirstUseEver);
    if (!Begin("Function Profiler", &open, ImGuiWindowFlags_MenuBar)) {
        End();
        return;
    }

    if (BeginMenuBar()) {
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
            if (MenuItem("Reset counters")) {
                HleProfiler::Reset();
                prev_calls.clear();
                last_refresh = -1.0;
                rows.clear();
            }
            EndMenu();
        }
        EndMenuBar();
    }

    const double now = GetTime();
    if (!paused && (last_refresh < 0.0 || now - last_refresh >= REFRESH_INTERVAL)) {
        Refresh(now);
    }

    const size_t shown = std::min(rows.size(), static_cast<size_t>(top_n));
    Text("Top %d | showing %zu of %zu active functions (%zu registered) | running for %.1fs%s",
         top_n, shown, rows.size(), static_cast<size_t>(registered_count), elapsed_seconds,
         paused ? " | PAUSED" : "");

    if (BeginTable("FunctionProfilerTable", 9,
                   ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                       ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                       ImGuiTableFlags_SizingFixedFit,
                   {0.0f, -GetTextLineHeightWithSpacing() * 2.0f})) {
        const auto desc = ImGuiTableColumnFlags_PreferSortDescending;
        TableSetupScrollFreeze(0, 1);
        TableSetupColumn("#", ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_WidthFixed,
                         32.0f);
        TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch, 0.0f, ColName);
        TableSetupColumn("Library", ImGuiTableColumnFlags_DefaultHide, 0.0f, ColLibrary);
        TableSetupColumn("Calls", desc, 0.0f, ColCalls);
        TableSetupColumn("Calls/s", desc, 0.0f, ColCallsPerSec);
        TableSetupColumn("Total (ms)", desc | ImGuiTableColumnFlags_DefaultSort, 0.0f, ColTotal);
        TableSetupColumn("Avg (us)", desc, 0.0f, ColAverage);
        TableSetupColumn("Max (us)", desc, 0.0f, ColMax);
        TableSetupColumn("Share", desc, 0.0f, ColShare);
        TableHeadersRow();
        SortRows();

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
            TableNextColumn();
            Text("%.2f", AverageNs(row) / 1e3);
            TableNextColumn();
            Text("%.2f", static_cast<double>(row.max_ns) / 1e3);
            TableNextColumn();
            Text("%.1f%%", row.share);
        }
        EndTable();
    }

    TextDisabled("Wall-clock time inside each HLE call (includes waits/sleeps). Game code itself\n"
                 "runs natively and is not listed.");
    End();
}

} // namespace Core::Devtools::Widget
