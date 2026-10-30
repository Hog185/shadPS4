// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>

#include <fmt/format.h>

#include "perf_report.h"

namespace Core::Devtools::PerfReport {

FrameStats ComputeFrameStats(std::span<const float> frame_ms) {
    FrameStats stats;
    stats.count = frame_ms.size();
    if (frame_ms.empty()) {
        return stats;
    }
    std::vector<float> sorted(frame_ms.begin(), frame_ms.end());
    std::ranges::sort(sorted);
    double sum = 0.0;
    for (const float value : sorted) {
        sum += value;
    }
    stats.avg_ms = sum / static_cast<double>(sorted.size());
    stats.min_ms = sorted.front();
    stats.max_ms = sorted.back();
    // Nearest-rank percentile.
    const auto rank = static_cast<size_t>(std::ceil(0.99 * static_cast<double>(sorted.size())));
    stats.p99_ms = sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
    return stats;
}

namespace {

constexpr size_t MAX_NAME_WIDTH = 72;

std::string Section(const std::string& title) {
    return fmt::format("\n{}\n{}\n", title, std::string(title.size(), '-'));
}

template <typename Rows>
size_t NameWidth(const Rows& rows) {
    size_t width = 4;
    for (const auto& row : rows) {
        width = std::max(width, std::min(row.name.size(), MAX_NAME_WIDTH));
    }
    return width;
}

std::string FormatCulprits(const std::vector<Culprit>& culprits) {
    std::string text;
    for (const auto& culprit : culprits) {
        text += fmt::format("{}{} {:.1f} ms", text.empty() ? "" : "; ", culprit.name, culprit.ms);
    }
    return text;
}

void AppendRows(std::string& out, const std::vector<Row>& rows, const char* time_label,
                bool with_cpu) {
    const size_t width = NameWidth(rows);
    out += fmt::format("{:>3}  {:<{}}  {:>10}  {:>11}", "#", "Name", width, "Calls", time_label);
    if (with_cpu) {
        out += fmt::format("  {:>10}", "CPU ms");
    }
    out += fmt::format("  {:>12}  {:>10}  {:>10}  {:>6}\n", "Calls/frame", "ms/frame", "Max us",
                       "Share");
    int index = 1;
    for (const auto& row : rows) {
        out += fmt::format("{:>3}  {:<{}}  {:>10}  {:>11.3f}", index++, row.name, width, row.calls,
                           row.total_ms);
        if (with_cpu) {
            out += fmt::format("  {:>10.3f}", row.cpu_ms);
        }
        out += fmt::format("  {:>12.2f}  {:>10.3f}  {:>10.1f}  {:>5.1f}%\n", row.calls_per_frame,
                           row.ms_per_frame, row.max_us, row.share);
    }
}

} // namespace

std::string FormatReport(const ReportData& data) {
    std::string out = "shadPS4 performance report\n==========================\n";
    out += data.system_info;
    if (!data.system_info.empty() && !data.system_info.ends_with('\n')) {
        out += '\n';
    }
    out += fmt::format("Captured: {}\n", data.timestamp);
    out += fmt::format("Profiled for {:.1f} s ({} frames)\n", data.profiled_seconds, data.frames);
    out += fmt::format("Settings: {}\n", data.settings);

    out += Section("Frame time");
    const auto& fs = data.frame_stats;
    if (fs.count == 0) {
        out += "No frames were recorded.\n";
    } else {
        out += fmt::format("{} frames: avg {:.2f} ms ({:.1f} FPS), min {:.2f} ms, max {:.2f} ms\n",
                           fs.count, fs.avg_ms, fs.avg_ms > 0.0 ? 1000.0 / fs.avg_ms : 0.0,
                           fs.min_ms, fs.max_ms);
        out += fmt::format("99th percentile {:.2f} ms (1% low {:.1f} FPS)\n", fs.p99_ms,
                           fs.p99_ms > 0.0 ? 1000.0 / fs.p99_ms : 0.0);
    }

    if (!data.hints.empty()) {
        out += Section("Hints");
        for (const auto& hint : data.hints) {
            out += fmt::format("- {}\n", hint);
        }
    }

    const bool with_cpu = !data.functions.empty() && data.functions.front().cpu_ms >= 0.0;
    out += Section(fmt::format("Functions ({} shown)", data.functions.size()));
    if (data.functions.empty()) {
        out += "None.\n";
    } else {
        AppendRows(out, data.functions, "Wall ms", with_cpu);
    }

    out += Section(fmt::format("GPU draws and dispatches ({} shown)", data.gpu.size()));
    if (data.gpu.empty()) {
        out += "None.\n";
    } else {
        AppendRows(out, data.gpu, "GPU ms", false);
    }

    out += Section(fmt::format("Threads ({} shown, 100% is one core)", data.threads.size()));
    if (data.threads.empty()) {
        out += "None.\n";
    } else {
        const size_t width = NameWidth(data.threads);
        out += fmt::format("{:>3}  {:<{}}  {:>7}  {:>7}  {:>8}\n", "#", "Thread", width, "CPU %",
                           "User %", "Kernel %");
        int index = 1;
        for (const auto& thread : data.threads) {
            out +=
                fmt::format("{:>3}  {:<{}}  {:>7.1f}  {:>7.1f}  {:>8.1f}\n", index++, thread.name,
                            width, thread.cpu_percent, thread.user_percent, thread.kernel_percent);
        }
    }

    out += Section(fmt::format("Stutters (frames slower than {:.0f} ms)", data.spike_threshold_ms));
    if (data.spikes.empty()) {
        out += "None.\n";
    } else {
        for (const auto& spike : data.spikes) {
            out += fmt::format("frame {} at {:.1f} s: {:.1f} ms\n", spike.frame, spike.at_seconds,
                               spike.frame_ms);
            if (!spike.cpu.empty()) {
                out += fmt::format("    cpu: {}\n", FormatCulprits(spike.cpu));
            }
            if (!spike.gpu.empty()) {
                out += fmt::format("    gpu: {}\n", FormatCulprits(spike.gpu));
            }
        }
    }
    return out;
}

} // namespace Core::Devtools::PerfReport
