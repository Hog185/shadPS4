// SPDX-License-Identifier: GPL-2.0-or-later
//
// See ue4_oodle_hook.h. Guest memory lives at its own addresses inside the emulator's process, and guest
// code is native x86-64 using the System V ABI, so a host function can be called from it directly.

#include "ue4_oodle_hook.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Core::Ue4OodleHook {

#if defined(__linux__)

namespace {

constexpr uint64_t DefaultOutgoing = 0x22ca5e0;
constexpr size_t PrologueSize = 17;
// push rbp; mov rbp,rsp; push r15; push r14; push r13; push r12; push rbx; sub rsp,0x48
constexpr uint8_t Prologue[PrologueSize] = {0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41,
                                            0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x48};
constexpr int64_t MaxBits = 8 * 4096;

std::atomic<bool> started{false};
std::mutex log_mutex;
FILE* log_file = nullptr;
std::atomic<uint64_t> logged{0};

bool ReadSelf(uint64_t addr, void* out, size_t n) {
    iovec local{out, n};
    iovec remote{reinterpret_cast<void*>(addr), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

// Permissions of the mapping that holds addr, as PROT_* flags, from /proc/self/maps; -1 if not found.
int ProtOf(uint64_t addr) {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        return -1;
    }
    char line[512];
    int prot = -1;
    while (std::fgets(line, sizeof(line), maps)) {
        uint64_t lo = 0, hi = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s", &lo, &hi, perms) == 3 && addr >= lo && addr < hi) {
            prot = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) |
                   (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    std::fclose(maps);
    return prot;
}

// Called from the game with the original rdi (the component) and rsi (the FBitWriter that holds the
// plaintext packet): +0x90 is the data pointer, +0xa0 the number of bits.
void OnOutgoing(void* /*component*/, void* writer) {
    if (!writer || !log_file) {
        return;
    }
    const uint8_t* data = *reinterpret_cast<uint8_t* const*>(static_cast<uint8_t*>(writer) + 0x90);
    const int64_t bits = *reinterpret_cast<int64_t*>(static_cast<uint8_t*>(writer) + 0xa0);
    if (!data || bits <= 0 || bits > MaxBits) {
        return;
    }
    const size_t bytes = static_cast<size_t>((bits + 7) / 8);
    static const char digits[] = "0123456789abcdef";
    std::string hex(bytes * 2, '0');
    for (size_t i = 0; i < bytes; ++i) {
        hex[2 * i] = digits[data[i] >> 4];
        hex[2 * i + 1] = digits[data[i] & 15];
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
    std::lock_guard<std::mutex> lock(log_mutex);
    std::fprintf(log_file,
                 "{\"bits\":%" PRId64 ",\"dir\":\"in\",\"hex\":\"%s\",\"len\":%zu,\"peer\":\"client\","
                 "\"src\":\"oodle-outgoing\",\"t\":%" PRId64 "}\n",
                 bits, hex.c_str(), bytes, static_cast<int64_t>(ms));
    std::fflush(log_file);
    ++logged;
}

// Replaces the first 17 bytes of fn with a 14 byte absolute jump (and 3 nops) to a trampoline that runs the
// displaced instructions, calls the hook with the original rdi/rsi (rdx is kept for the function), and
// jumps back to fn + 17.
bool Install(uint8_t* fn) {
    uint8_t* tramp = static_cast<uint8_t*>(
        mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (tramp == MAP_FAILED) {
        return false;
    }
    uint8_t* p = tramp;
    auto put = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) {
            *p++ = b;
        }
    };
    auto put64 = [&](uint64_t v) {
        std::memcpy(p, &v, 8);
        p += 8;
    };
    std::memcpy(p, fn, PrologueSize); // displaced instructions, all position independent
    p += PrologueSize;
    put({0x57, 0x56, 0x52});       // push rdi; push rsi; push rdx
    put({0x48, 0x83, 0xec, 0x08}); // sub rsp, 8  (rsp is 16-byte aligned after the displaced code)
    put({0x48, 0xb8});             // movabs rax, OnOutgoing
    put64(reinterpret_cast<uint64_t>(&OnOutgoing));
    put({0xff, 0xd0});             // call rax
    put({0x48, 0x83, 0xc4, 0x08}); // add rsp, 8
    put({0x5a, 0x5e, 0x5f});       // pop rdx; pop rsi; pop rdi
    put({0xff, 0x25, 0, 0, 0, 0}); // jmp [rip+0]
    put64(reinterpret_cast<uint64_t>(fn + PrologueSize));

    const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
    const uint64_t first = reinterpret_cast<uint64_t>(fn) & ~(page - 1);
    const uint64_t last = (reinterpret_cast<uint64_t>(fn) + PrologueSize - 1) & ~(page - 1);
    const size_t len = static_cast<size_t>(last - first + page);
    int original = ProtOf(first);
    if (original < 0) {
        original = PROT_READ | PROT_EXEC;
    }
    if (mprotect(reinterpret_cast<void*>(first), len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return false;
    }
    fn[0] = 0xff; // jmp [rip+0]
    fn[1] = 0x25;
    fn[2] = fn[3] = fn[4] = fn[5] = 0;
    const uint64_t target = reinterpret_cast<uint64_t>(tramp);
    std::memcpy(fn + 6, &target, 8);
    fn[14] = fn[15] = fn[16] = 0x90;
    mprotect(reinterpret_cast<void*>(first), len, original | PROT_EXEC);
    __builtin___clear_cache(reinterpret_cast<char*>(fn), reinterpret_cast<char*>(fn + PrologueSize));
    return true;
}

void Run(std::string path, uint64_t addr) {
    // Wait for the game's code to be loaded and the bytes there to be readable (never touch unmapped memory).
    uint8_t seen[PrologueSize];
    for (int i = 0; i < 6000; ++i) { // up to 10 minutes
        if (ReadSelf(addr, seen, PrologueSize) && seen[0] != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ReadSelf(addr, seen, PrologueSize) || std::memcmp(seen, Prologue, PrologueSize) != 0) {
        std::fprintf(stderr,
                     "[ue4oodle] the bytes at 0x%" PRIx64 " are not OodleHandlerComponent::Outgoing's "
                     "prologue (different build?), nothing patched\n",
                     addr);
        return;
    }
    log_file = std::fopen(path.c_str(), "a");
    if (!log_file) {
        std::fprintf(stderr, "[ue4oodle] cannot open %s\n", path.c_str());
        return;
    }
    if (!Install(reinterpret_cast<uint8_t*>(addr))) {
        std::fprintf(stderr, "[ue4oodle] patching 0x%" PRIx64 " failed\n", addr);
        return;
    }
    std::fprintf(stderr, "[ue4oodle] hooked 0x%" PRIx64 ", logging client packets to %s\n", addr, path.c_str());
}

} // namespace

void Start() {
    const char* path = std::getenv("SHADPS4_UE4OODLE_LOG");
    if (!path || !*path || started.exchange(true)) {
        return;
    }
    uint64_t addr = DefaultOutgoing;
    if (const char* a = std::getenv("SHADPS4_UE4OODLE_ADDR")) {
        addr = std::strtoull(a, nullptr, 16);
    }
    std::thread(Run, std::string(path), addr).detach();
}

#else // not Linux

void Start() {
    if (const char* path = std::getenv("SHADPS4_UE4OODLE_LOG"); path && *path) {
        std::fprintf(stderr, "[ue4oodle] only implemented on Linux\n");
    }
}

#endif

} // namespace Core::Ue4OodleHook
