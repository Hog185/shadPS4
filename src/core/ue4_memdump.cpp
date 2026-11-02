// SPDX-License-Identifier: GPL-2.0-or-later
//
// See ue4_memdump.h. No dependencies on the rest of the emulator: guest memory lives at its own
// addresses inside the emulator's process, so the process reads its own memory.
//
// File format (little endian), read by tools/ue4_dump.py:
//   8 bytes   "UE4DMP01"
//   8 bytes   number of regions (informational; readers go on until the end of the file)
//   then per region: u64 base, u64 size, u32 protection (bit0 r, bit1 w, bit2 x), u32 0, <size bytes>
// Regions are runs of readable memory; unreadable pages are left out.

#include "ue4_memdump.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Core::Ue4MemDump {

namespace {

constexpr char Magic[8] = {'U', 'E', '4', 'D', 'M', 'P', '0', '1'};
constexpr size_t ReadChunk = 1u << 20; // read in 1 MiB pieces, salvage page by page on failure
constexpr size_t PageSize = 4096;
constexpr size_t RunFlushSize = 64u << 20;

std::mutex g_mutex;
std::atomic<bool> g_started{false};

uint64_t EnvU64(const char* name, uint64_t fallback) {
    const char* v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return fallback;
    }
    return std::strtoull(v, nullptr, 0); // accepts 0x...
}

std::filesystem::path DumpDir() {
    const char* v = std::getenv("SHADPS4_UE4DUMP_DIR");
    return (v != nullptr && *v != '\0') ? std::filesystem::path(v) : std::filesystem::path();
}

// Reads n bytes of this process's memory; false if any page in the range is unreadable.
bool ReadSelf(uint64_t addr, void* dst, size_t n) {
#ifdef _WIN32
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, n, &got) &&
           got == n;
#else
    iovec local{dst, n};
    iovec remote{reinterpret_cast<void*>(addr), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
#endif
}

struct Region {
    uint64_t base;
    uint64_t size;
    uint32_t prot;
};

// The readable regions of this process that intersect [lo, hi).
std::vector<Region> ReadableRegions(uint64_t lo, uint64_t hi) {
    std::vector<Region> out;
#ifdef _WIN32
    uint64_t addr = lo;
    while (addr < hi) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const uint64_t base = reinterpret_cast<uint64_t>(mbi.BaseAddress);
        const uint64_t end = base + mbi.RegionSize;
        const bool readable = mbi.State == MEM_COMMIT && mbi.Protect != 0 &&
                              (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
        if (readable) {
            const uint64_t a = base < lo ? lo : base;
            const uint64_t b = end > hi ? hi : end;
            if (b > a) {
                out.push_back({a, b - a, 1u});
            }
        }
        if (end <= addr) {
            break;
        }
        addr = end;
    }
#else
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) {
        return out;
    }
    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long start = 0, end = 0;
        char perms[8] = {};
        int consumed = 0;
        if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", &start, &end, perms, &consumed) < 3) {
            continue;
        }
        const char* path = line + consumed;
        if (perms[0] != 'r' || std::strncmp(path, "/dev/", 5) == 0) {
            continue; // unreadable (the emulator's reserved but unmapped guest space) or a device
        }
        const uint64_t a = start < lo ? lo : start;
        const uint64_t b = end > hi ? hi : end;
        if (b > a) {
            const uint32_t prot = 1u | (perms[1] == 'w' ? 2u : 0u) | (perms[2] == 'x' ? 4u : 0u);
            out.push_back({a, b - a, prot});
        }
    }
    std::fclose(maps);
#endif
    return out;
}

// Collects consecutive readable bytes into runs and writes each run as one region record.
class Sink {
public:
    explicit Sink(std::FILE* file) : file_(file) {}

    void Append(uint64_t addr, const uint8_t* data, size_t n, uint32_t prot) {
        if (!run_.empty() && (addr != runBase_ + run_.size() || prot != runProt_)) {
            Flush();
        }
        if (run_.empty()) {
            runBase_ = addr;
            runProt_ = prot;
        }
        run_.insert(run_.end(), data, data + n);
        total_ += n;
        if (run_.size() >= RunFlushSize) {
            Flush();
        }
    }

    void Flush() {
        if (run_.empty()) {
            return;
        }
        const uint64_t size = run_.size();
        const uint32_t pad = 0;
        std::fwrite(&runBase_, 8, 1, file_);
        std::fwrite(&size, 8, 1, file_);
        std::fwrite(&runProt_, 4, 1, file_);
        std::fwrite(&pad, 4, 1, file_);
        std::fwrite(run_.data(), 1, run_.size(), file_);
        run_.clear();
        ++regions_;
    }

    uint64_t total() const { return total_; }
    uint64_t regions() const { return regions_; }

private:
    std::FILE* file_;
    std::vector<uint8_t> run_;
    uint64_t runBase_ = 0;
    uint32_t runProt_ = 0;
    uint64_t total_ = 0;
    uint64_t regions_ = 0;
};

bool IsZeroPage(const uint8_t* p, size_t n) {
    static const uint8_t zeros[PageSize] = {};
    return std::memcmp(p, zeros, n) == 0;
}

// Like Sink::Append, but leaves out pages that are entirely zero.
void AppendNonZero(Sink& sink, uint64_t addr, const uint8_t* data, size_t n, uint32_t prot) {
    for (size_t p = 0; p < n; p += PageSize) {
        const size_t m = std::min(PageSize, n - p);
        if (!IsZeroPage(data + p, m)) {
            sink.Append(addr + p, data + p, m, prot);
        }
    }
}

void Watcher(std::filesystem::path dir) {
    const std::filesystem::path trigger = dir / "trigger";
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::error_code ec;
        if (std::filesystem::exists(trigger, ec)) {
            std::filesystem::remove(trigger, ec);
            const std::string path = DumpNow();
            if (path.empty()) {
                std::fprintf(stderr, "[ue4dump] dump failed\n");
            }
        }
    }
}

} // namespace

std::string DumpNow() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    const std::filesystem::path dir = DumpDir();
    if (dir.empty()) {
        return {};
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    const uint64_t lo = EnvU64("SHADPS4_UE4DUMP_MIN", 0x400000ULL);
    const uint64_t hi = EnvU64("SHADPS4_UE4DUMP_MAX", 0xFC00000000ULL);
    const uint64_t cap = EnvU64("SHADPS4_UE4DUMP_MAX_BYTES", 8ULL << 30);

    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    const std::filesystem::path path = dir / (std::string("ue4dump-") + stamp + ".bin");

    std::FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        std::fprintf(stderr, "[ue4dump] cannot create %s\n", path.string().c_str());
        return {};
    }
    std::fwrite(Magic, 1, sizeof(Magic), file);
    const uint64_t countPlaceholder = 0;
    std::fwrite(&countPlaceholder, 8, 1, file);

    Sink sink(file);
    std::vector<uint8_t> buffer(ReadChunk);
    const std::vector<Region> regions = ReadableRegions(lo, hi);
    std::fprintf(stderr, "[ue4dump] %zu readable regions in 0x%" PRIx64 "-0x%" PRIx64 "\n", regions.size(),
                 lo, hi);
    bool capped = false;
    for (const Region& r : regions) {
        for (uint64_t off = 0; off < r.size && !capped; off += ReadChunk) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(ReadChunk, r.size - off));
            if (ReadSelf(r.base + off, buffer.data(), n)) {
                AppendNonZero(sink, r.base + off, buffer.data(), n, r.prot);
            } else {
                for (size_t p = 0; p < n; p += PageSize) { // salvage what can be read
                    const size_t m = std::min(PageSize, n - p);
                    if (ReadSelf(r.base + off + p, buffer.data(), m)) {
                        AppendNonZero(sink, r.base + off + p, buffer.data(), m, r.prot);
                    }
                }
            }
            capped = sink.total() >= cap;
        }
        if (capped) {
            std::fprintf(stderr, "[ue4dump] stopped at the %" PRIu64 " MiB limit\n", cap >> 20);
            break;
        }
    }
    sink.Flush();
    std::fseek(file, 8, SEEK_SET);
    const uint64_t count = sink.regions();
    std::fwrite(&count, 8, 1, file);
    std::fclose(file);
    std::fprintf(stderr, "[ue4dump] wrote %s: %" PRIu64 " MiB in %" PRIu64 " regions\n",
                 path.string().c_str(), sink.total() >> 20, count);
    return path.string();
}

void Start() {
    const std::filesystem::path dir = DumpDir();
    if (dir.empty() || g_started.exchange(true)) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::fprintf(stderr, "[ue4dump] armed: create %s to take a dump\n", (dir / "trigger").string().c_str());
    std::thread(Watcher, dir).detach();
}

} // namespace Core::Ue4MemDump
