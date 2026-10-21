// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>

#include "common/thread.h"
#include "thread_usage.h"

#ifdef __linux__
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace Core::Devtools::ThreadUsage {

namespace {

struct RawThread {
    u64 id;
    std::string name;
    u64 user_ns;
    u64 kernel_ns;
};

#ifdef __linux__

std::vector<RawThread> EnumerateThreads() {
    std::vector<RawThread> threads;
    const u64 ns_per_tick = 1'000'000'000ull / static_cast<u64>(sysconf(_SC_CLK_TCK));
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        u64 tid = 0;
        const std::string tid_str = entry.path().filename().string();
        if (std::from_chars(tid_str.data(), tid_str.data() + tid_str.size(), tid).ec !=
            std::errc{}) {
            continue;
        }
        std::ifstream stat_file{entry.path() / "stat"};
        std::string line;
        if (!std::getline(stat_file, line)) {
            continue; // thread exited while enumerating
        }
        // The name sits in parentheses and may itself contain spaces or parentheses.
        const auto open = line.find('(');
        const auto close = line.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open) {
            continue;
        }
        std::string comm = line.substr(open + 1, close - open - 1);
        std::istringstream fields{line.substr(close + 1)};
        // Fields after the name start at field 3 (state); utime and stime are fields 14 and 15.
        std::string field;
        u64 utime = 0;
        u64 stime = 0;
        for (int index = 3; fields >> field; ++index) {
            if (index == 14) {
                utime = std::strtoull(field.c_str(), nullptr, 10);
            } else if (index == 15) {
                stime = std::strtoull(field.c_str(), nullptr, 10);
                break;
            }
        }
        // The kernel truncates names to 15 characters, prefer the full one if it matches.
        std::string full_name = Common::GetRecordedThreadName(tid);
        const bool matches = !full_name.empty() && full_name.compare(0, comm.size(), comm) == 0;
        threads.push_back({tid, matches ? std::move(full_name) : std::move(comm),
                           utime * ns_per_tick, stime * ns_per_tick});
    }
    return threads;
}

#elif defined(_WIN32)

u64 FileTimeTo100ns(const FILETIME& ft) {
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string WideToUtf8(const wchar_t* wide) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::vector<RawThread> EnumerateThreads() {
    std::vector<RawThread> threads;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return threads;
    }
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
        if (entry.th32OwnerProcessID != pid) {
            continue;
        }
        const HANDLE handle =
            OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
        if (!handle) {
            continue;
        }
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (GetThreadTimes(handle, &creation, &exit, &kernel, &user)) {
            std::string name;
            PWSTR description = nullptr;
            if (SUCCEEDED(GetThreadDescription(handle, &description)) && description) {
                name = WideToUtf8(description);
                LocalFree(description);
            }
            // FILETIME units are 100 ns.
            threads.push_back({entry.th32ThreadID, std::move(name), FileTimeTo100ns(user) * 100,
                               FileTimeTo100ns(kernel) * 100});
        }
        CloseHandle(handle);
    }
    CloseHandle(snapshot);
    return threads;
}

#else

std::vector<RawThread> EnumerateThreads() {
    return {};
}

#endif

} // namespace

bool IsSupported() {
#if defined(__linux__) || defined(_WIN32)
    return true;
#else
    return false;
#endif
}

void ThreadSampler::Reset() {
    previous.clear();
    has_last_sample = false;
}

std::vector<ThreadInfo> ThreadSampler::Sample() {
    const auto now = std::chrono::steady_clock::now();
    const auto raw = EnumerateThreads();
    const double elapsed_ns =
        has_last_sample
            ? static_cast<double>(
                  std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_sample).count())
            : 0.0;

    std::vector<ThreadInfo> result;
    std::unordered_map<u64, Previous> current;
    current.reserve(raw.size());
    for (const auto& thread : raw) {
        current[thread.id] = {thread.user_ns, thread.kernel_ns};
        const auto it = previous.find(thread.id);
        if (it == previous.end() || elapsed_ns <= 0.0) {
            continue;
        }
        const double user_ns =
            static_cast<double>(thread.user_ns - std::min(thread.user_ns, it->second.user_ns));
        const double kernel_ns = static_cast<double>(
            thread.kernel_ns - std::min(thread.kernel_ns, it->second.kernel_ns));
        result.push_back({thread.id, thread.name.empty() ? "(unnamed)" : thread.name,
                          (user_ns + kernel_ns) * 100.0 / elapsed_ns, user_ns * 100.0 / elapsed_ns,
                          kernel_ns * 100.0 / elapsed_ns,
                          static_cast<double>(thread.user_ns + thread.kernel_ns) / 1e9});
    }
    previous = std::move(current);
    last_sample = now;
    has_last_sample = true;
    return result;
}

} // namespace Core::Devtools::ThreadUsage
