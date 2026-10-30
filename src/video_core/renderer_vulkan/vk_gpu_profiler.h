// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "common/types.h"
#include "core/devtools/hle_profiler.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Pipeline;

// A named group of GPU work, such as one pair of vertex and pixel shaders.
struct GpuLabel {
    Core::Devtools::HleProfiler::FunctionStats stats;
    std::string name;
};

// Measures GPU time with timestamp queries while the devtools profiler window is open. Every
// scheduler session (one command buffer) gets a block of queries: the first two bracket the whole
// command buffer, the rest are handed out in pairs to GpuScope. Results are read back once the
// submission has finished on the GPU, so nothing ever waits.
class GpuProfiler {
public:
    GpuProfiler(vk::Device device, vk::PhysicalDevice physical_device, u32 queue_family);
    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    // Name shown for the command buffers of this scheduler.
    void SetName(const char* name);

    // How many scopes fit in one command buffer. Must be set before profiling starts.
    void SetScopeCapacity(u32 scopes_per_session);

    // Right after the command buffer begins and right before it ends (outside of rendering).
    void BeginSession(vk::CommandBuffer cmdbuf);
    void EndSession(vk::CommandBuffer cmdbuf);

    // After the session was submitted, signalling the given timeline tick.
    void OnSubmit(u64 tick);

    // Reads back the sessions whose tick is_free reports as finished.
    template <typename IsFree>
    void Collect(IsFree&& is_free) {
        if (pending_count.load(std::memory_order_relaxed) == 0) {
            return;
        }
        std::scoped_lock lock{mutex};
        while (!pending.empty() && is_free(pending.front().tick)) {
            ReadBack(pending.front());
            free_slots.push_back(pending.front().slot);
            pending.pop_front();
            pending_count.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    static constexpr u64 INVALID_TOKEN = ~0ull;

    [[nodiscard]] bool IsRecording() const {
        return recording.load(std::memory_order_relaxed);
    }

    // Writes the start timestamp of a scope. Returns INVALID_TOKEN when nothing is timed.
    [[nodiscard]] u64 BeginScope(vk::CommandBuffer cmdbuf, GpuLabel* label);
    void EndScope(vk::CommandBuffer cmdbuf, u64 token);

private:
    struct ScopeRecord {
        u32 query; // first of the two queries, relative to the session block
        GpuLabel* label;
    };
    struct Batch {
        u32 slot{};
        u32 serial{};
        u32 next_query{};
        u64 tick{};
        std::vector<ScopeRecord> scopes;
    };

    bool EnsurePool();
    void ReadBack(const Batch& batch);

    const vk::Device device;
    bool supported{};
    u64 timestamp_mask{};
    double ns_per_tick{};

    std::mutex mutex;
    vk::QueryPool pool{};
    u32 scope_capacity{};
    u32 queries_per_session{};
    u32 next_serial{};
    std::vector<u32> free_slots;
    Batch open;
    bool has_open{};
    std::vector<Batch> closed;
    std::deque<Batch> pending;
    std::atomic<u32> pending_count{0};
    std::atomic<bool> recording{false};
    Core::Devtools::HleProfiler::FunctionStats* session_stats{};
    std::string name{"Scheduler"};
};

// Times the GPU work recorded during its lifetime, e.g. a draw call.
class GpuScope {
public:
    // The label is built from the shaders of the pipeline.
    GpuScope(GpuProfiler& profiler, vk::CommandBuffer cmdbuf, const Pipeline& pipeline,
             const char* kind);
    ~GpuScope();

    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    GpuProfiler& profiler;
    vk::CommandBuffer cmdbuf;
    u64 token;
};

} // namespace Vulkan
