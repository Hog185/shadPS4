// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>

#include "common/hash.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"

namespace Vulkan {

namespace HleProfiler = Core::Devtools::HleProfiler;

namespace {

// Command buffers that can be waiting for the GPU while new ones are recorded.
constexpr u32 MAX_SESSIONS_IN_FLIGHT = 8;
// Whole-command-buffer begin and end.
constexpr u32 SESSION_QUERIES = 2;

struct LabelTable {
    std::mutex mutex;
    std::unordered_map<u64, GpuLabel*> labels;
};

LabelTable& GetLabelTable() {
    // Never destroyed: the profiler list keeps pointing at the labels until the process exits.
    static auto* table = new LabelTable;
    return *table;
}

const char* StageName(Shader::SwStage stage) {
    switch (stage) {
    case Shader::SwStage::Vertex:
        return "VS";
    case Shader::SwStage::TessellationControl:
        return "HS";
    case Shader::SwStage::TessellationEval:
        return "DS";
    case Shader::SwStage::Geometry:
        return "GS";
    case Shader::SwStage::Fragment:
        return "PS";
    case Shader::SwStage::Compute:
        return "CS";
    default:
        return "??";
    }
}

GpuLabel* GetLabel(const Pipeline& pipeline, const char* kind) {
    static constexpr std::array ORDER = {
        Shader::SwStage::Vertex,           Shader::SwStage::TessellationControl,
        Shader::SwStage::TessellationEval, Shader::SwStage::Geometry,
        Shader::SwStage::Fragment,         Shader::SwStage::Compute,
    };
    std::array<const Shader::Info*, Shader::MaxStageTypes> by_stage{};
    for (const Shader::Info* info : pipeline.GetStages()) {
        if (info) {
            by_stage[static_cast<u32>(info->sw_stage)] = info;
        }
    }

    u64 key = std::hash<std::string_view>{}(kind);
    for (const auto stage : ORDER) {
        if (const auto* info = by_stage[static_cast<u32>(stage)]) {
            key = HashCombine(key, info->pgm_hash);
        }
    }

    auto& table = GetLabelTable();
    std::scoped_lock lock{table.mutex};
    auto& label = table.labels[key];
    if (!label) {
        label = new GpuLabel;
        label->name = kind;
        for (const auto stage : ORDER) {
            if (const auto* info = by_stage[static_cast<u32>(stage)]) {
                label->name += fmt::format(" {}:{:#x}", StageName(stage), info->pgm_hash);
            }
        }
        HleProfiler::Register(&label->stats, label->name.c_str(), "gpu",
                              HleProfiler::Category::Gpu);
    }
    return label;
}

} // namespace

GpuProfiler::GpuProfiler(vk::Device device_, vk::PhysicalDevice physical_device, u32 queue_family)
    : device{device_} {
    const auto limits = physical_device.getProperties().limits;
    const auto queue_families = physical_device.getQueueFamilyProperties();
    const u32 valid_bits =
        queue_family < queue_families.size() ? queue_families[queue_family].timestampValidBits : 0;
    supported = valid_bits > 0 && limits.timestampPeriod > 0.0f;
    timestamp_mask = valid_bits >= 64 ? ~0ull : (1ull << valid_bits) - 1;
    ns_per_tick = limits.timestampPeriod;
    if (!supported) {
        HleProfiler::g_state.gpu_status.store(2, std::memory_order_relaxed);
    }
    // Never destroyed, see GetLabelTable.
    session_stats = new HleProfiler::FunctionStats;
}

GpuProfiler::~GpuProfiler() {
    if (pool) {
        device.destroyQueryPool(pool);
    }
}

void GpuProfiler::SetName(const char* new_name) {
    std::scoped_lock lock{mutex};
    name = new_name;
}

void GpuProfiler::SetScopeCapacity(u32 scopes_per_session) {
    std::scoped_lock lock{mutex};
    if (!pool) {
        scope_capacity = scopes_per_session;
    }
}

bool GpuProfiler::EnsurePool() {
    if (pool) {
        return true;
    }
    if (!supported) {
        return false;
    }
    queries_per_session = SESSION_QUERIES + scope_capacity * 2;
    const vk::QueryPoolCreateInfo pool_info = {
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = queries_per_session * MAX_SESSIONS_IN_FLIGHT,
    };
    const auto [result, new_pool] = device.createQueryPool(pool_info);
    if (result != vk::Result::eSuccess) {
        supported = false;
        HleProfiler::g_state.gpu_status.store(2, std::memory_order_relaxed);
        return false;
    }
    pool = new_pool;
    for (u32 slot = MAX_SESSIONS_IN_FLIGHT; slot > 0; --slot) {
        free_slots.push_back(slot - 1);
    }
    // Never freed, the profiler list keeps pointing at the name until the process exits.
    HleProfiler::Register(session_stats, (new std::string(name))->c_str(), "gpu",
                          HleProfiler::Category::GpuQueue);
    HleProfiler::g_state.gpu_status.store(1, std::memory_order_relaxed);
    return true;
}

void GpuProfiler::BeginSession(vk::CommandBuffer cmdbuf) {
    if (!HleProfiler::IsEnabled()) {
        return;
    }
    std::scoped_lock lock{mutex};
    if (has_open || !EnsurePool() || free_slots.empty()) {
        return;
    }
    open = Batch{};
    open.slot = free_slots.back();
    open.serial = ++next_serial;
    open.next_query = SESSION_QUERIES;
    free_slots.pop_back();
    has_open = true;

    const u32 first = open.slot * queries_per_session;
    cmdbuf.resetQueryPool(pool, first, queries_per_session);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool, first);
    recording.store(true, std::memory_order_relaxed);
}

void GpuProfiler::EndSession(vk::CommandBuffer cmdbuf) {
    std::scoped_lock lock{mutex};
    if (!has_open) {
        return;
    }
    recording.store(false, std::memory_order_relaxed);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool,
                          open.slot * queries_per_session + 1);
    closed.push_back(std::move(open));
    has_open = false;
}

void GpuProfiler::OnSubmit(u64 tick) {
    std::scoped_lock lock{mutex};
    for (auto& batch : closed) {
        batch.tick = tick;
        pending.push_back(std::move(batch));
        pending_count.fetch_add(1, std::memory_order_relaxed);
    }
    closed.clear();
}

u64 GpuProfiler::BeginScope(vk::CommandBuffer cmdbuf, GpuLabel* label) {
    std::scoped_lock lock{mutex};
    if (!has_open) {
        return INVALID_TOKEN;
    }
    if (open.next_query + 2 > queries_per_session) {
        HleProfiler::g_state.gpu_dropped_scopes.fetch_add(1, std::memory_order_relaxed);
        return INVALID_TOKEN;
    }
    const u32 query = open.next_query;
    open.next_query += 2;
    open.scopes.push_back({query, label});
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, pool,
                          open.slot * queries_per_session + query);
    return (static_cast<u64>(open.serial) << 32) | query;
}

void GpuProfiler::EndScope(vk::CommandBuffer cmdbuf, u64 token) {
    if (token == INVALID_TOKEN) {
        return;
    }
    std::scoped_lock lock{mutex};
    // The command buffer must still be the one the scope started in.
    if (!has_open || open.serial != static_cast<u32>(token >> 32)) {
        return;
    }
    const u32 query = static_cast<u32>(token & 0xFFFFFFFF);
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, pool,
                          open.slot * queries_per_session + query + 1);
}

void GpuProfiler::ReadBack(const Batch& batch) {
    // Each query yields its value followed by whether it was available.
    std::vector<u64> data(static_cast<size_t>(batch.next_query) * 2);
    const vk::Result result = device.getQueryPoolResults(
        pool, batch.slot * queries_per_session, batch.next_query, data.size() * sizeof(u64),
        data.data(), 2 * sizeof(u64),
        vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWithAvailability);
    if (result != vk::Result::eSuccess && result != vk::Result::eNotReady) {
        return;
    }
    if (!HleProfiler::IsEnabled()) {
        return;
    }

    const auto duration_ns = [&](u32 begin_query) -> std::optional<u64> {
        const u32 end_query = begin_query + 1;
        if (data[end_query * 2 + 1] == 0 || data[begin_query * 2 + 1] == 0) {
            return std::nullopt;
        }
        const u64 ticks = (data[end_query * 2] - data[begin_query * 2]) & timestamp_mask;
        return static_cast<u64>(static_cast<double>(ticks) * ns_per_tick);
    };

    if (const auto session_ns = duration_ns(0)) {
        HleProfiler::RecordSample(*session_stats, *session_ns);
    }
    for (const auto& scope : batch.scopes) {
        if (const auto scope_ns = duration_ns(scope.query)) {
            HleProfiler::RecordSample(scope.label->stats, *scope_ns);
        }
    }
}

GpuScope::GpuScope(GpuProfiler& profiler_, vk::CommandBuffer cmdbuf_, const Pipeline& pipeline,
                   const char* kind)
    : profiler{profiler_}, cmdbuf{cmdbuf_}, token{GpuProfiler::INVALID_TOKEN} {
    if (profiler.IsRecording()) {
        token = profiler.BeginScope(cmdbuf, GetLabel(pipeline, kind));
    }
}

GpuScope::~GpuScope() {
    profiler.EndScope(cmdbuf, token);
}

} // namespace Vulkan
