// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "core/devtools/hle_profiler.h"

namespace Core::Devtools::Widget {

class FunctionProfiler {
public:
    void Draw();

    bool open = false;
    int top_n = 25; // 25, 50 or 75

private:
    struct Row {
        const char* name;
        const char* library;
        u64 calls;
        u64 total_ns;
        u64 max_ns;
        double calls_per_sec;
        double share; // percent of total profiled time
    };

    void Refresh(double now);
    void SortRows();

    std::vector<Row> rows;
    std::unordered_map<const HleProfiler::FunctionStats*, u64> prev_calls;
    u64 registered_count = 0;
    double elapsed_seconds = 0.0;
    double last_refresh = -1.0;
    bool paused = false;
};

} // namespace Core::Devtools::Widget
