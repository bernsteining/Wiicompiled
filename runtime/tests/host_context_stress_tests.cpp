// Stress test for every HostContext backend. host_context_tests.cpp checks the basic contract;
// this one checks what a guest scheduler leans on in practice:
//   * symmetric switches (worker -> worker, not only through the scheduler),
//   * address-taken locals and deep recursion staying intact across switches, which is what
//     exercises a backend that keeps a separate C shadow stack (WebAssembly/JSPI),
//   * C++ exceptions thrown and caught inside a context after it was resumed,
//   * destroying a context that is parked in the middle of a switch.

#include "host_context.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {

HostContext::Handle g_scheduler = nullptr;
constexpr int kWorkers = 3;
HostContext::Handle g_workers[kWorkers]{};
int g_turns[kWorkers]{};
constexpr int kRounds = 2000;

void Fail(const char* what)
{
    std::fprintf(stderr, "host_context_stress_tests: %s\n", what);
    std::abort();
}

// Each worker keeps a buffer on its own stack, hands control straight to the next worker, and
// checks the buffer is untouched when it comes back. The last worker of a round returns to the
// scheduler.
void RingWorker(void* argument)
{
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(argument));
    volatile uint32_t buffer[64];
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < 64; ++i) {
            buffer[i] = static_cast<uint32_t>(index * 100000 + round * 64 + i);
        }
        if (!HostContext::IsCurrent(g_workers[index])) {
            Fail("ring worker is not current");
        }
        ++g_turns[index];
        HostContext::Switch(index + 1 < kWorkers ? g_workers[index + 1] : g_scheduler);
        for (int i = 0; i < 64; ++i) {
            if (buffer[i] != static_cast<uint32_t>(index * 100000 + round * 64 + i)) {
                Fail("stack buffer changed across a switch");
            }
        }
    }
    HostContext::Switch(g_scheduler);
    Fail("ring worker resumed after finishing");
}

// Recursion with a frame-local array: 1500 levels x ~272 bytes is ~400 KiB of C stack, and the
// context switches away at the bottom.
HostContext::Handle g_deep = nullptr;
uint64_t Recurse(int depth)
{
    volatile uint8_t local[256];
    for (int i = 0; i < 256; ++i) {
        local[i] = static_cast<uint8_t>(depth + i);
    }
    uint64_t sum = 0;
    if (depth == 0) {
        HostContext::Switch(g_scheduler);
    } else {
        sum = Recurse(depth - 1);
    }
    for (int i = 0; i < 256; ++i) {
        if (local[i] != static_cast<uint8_t>(depth + i)) {
            Fail("recursion frame changed across a switch");
        }
        sum += local[i];
    }
    return sum;
}

uint64_t g_deepSum = 0;
void DeepWorker(void*)
{
    g_deepSum = Recurse(1500);
    HostContext::Switch(g_scheduler);
    Fail("deep worker resumed after finishing");
}

HostContext::Handle g_thrower = nullptr;
int g_caught = 0;
void ThrowingWorker(void*)
{
    for (int i = 0; i < 3; ++i) {
        HostContext::Switch(g_scheduler);
        try {
            throw std::runtime_error("expected");
        } catch (const std::runtime_error&) {
            ++g_caught;
        }
    }
    HostContext::Switch(g_scheduler);
    Fail("throwing worker resumed after finishing");
}

HostContext::Handle g_parked = nullptr;
bool g_parkedRan = false;
void ParkedWorker(void*)
{
    g_parkedRan = true;
    HostContext::Switch(g_scheduler);
    Fail("destroyed worker was resumed");
}

} // namespace

int main()
{
    if (!HostContext::InitializeScheduler(&g_scheduler)) {
        Fail("scheduler init");
    }

    for (int i = 0; i < kWorkers; ++i) {
        g_workers[i] = HostContext::Create(256 * 1024, RingWorker, reinterpret_cast<void*>(intptr_t{i}));
        if (!g_workers[i]) {
            Fail("create ring worker");
        }
    }
    volatile uint32_t schedulerBuffer[32];
    for (int round = 0; round < kRounds; ++round) {
        for (int i = 0; i < 32; ++i) schedulerBuffer[i] = static_cast<uint32_t>(round ^ i);
        HostContext::Switch(g_workers[0]);
        if (!HostContext::IsCurrent(g_scheduler)) Fail("scheduler is not current after a round");
        for (int i = 0; i < 32; ++i) {
            if (schedulerBuffer[i] != static_cast<uint32_t>(round ^ i)) Fail("scheduler stack changed");
        }
    }
    for (int i = 0; i < kWorkers; ++i) {
        if (g_turns[i] != kRounds) Fail("ring turn count");
        HostContext::Switch(g_workers[i]);  // let each one finish its loop and park
        HostContext::Destroy(g_workers[i]);
    }

    g_deep = HostContext::Create(1024 * 1024, DeepWorker, nullptr);
    HostContext::Switch(g_deep);  // parks at the bottom of the recursion
    HostContext::Switch(g_deep);  // unwinds and finishes
    uint64_t expected = 0;
    for (int depth = 0; depth <= 1500; ++depth)
        for (int i = 0; i < 256; ++i) expected += static_cast<uint8_t>(depth + i);
    if (g_deepSum != expected) Fail("deep recursion result");
    HostContext::Destroy(g_deep);

    g_thrower = HostContext::Create(64 * 1024, ThrowingWorker, nullptr);
    for (int i = 0; i < 4; ++i) HostContext::Switch(g_thrower);
    if (g_caught != 3) Fail("exceptions inside a context");
    HostContext::Destroy(g_thrower);

    // Destroy a context while it is parked inside Switch, then keep using the scheduler.
    g_parked = HostContext::Create(64 * 1024, ParkedWorker, nullptr);
    HostContext::Switch(g_parked);
    if (!g_parkedRan) Fail("parked worker did not run");
    HostContext::Destroy(g_parked);
    HostContext::Handle again = HostContext::Create(64 * 1024, ParkedWorker, nullptr);
    g_parkedRan = false;
    HostContext::Switch(again);
    if (!g_parkedRan) Fail("context created after a destroy did not run");
    HostContext::Destroy(again);

    HostContext::ShutdownScheduler(g_scheduler);
    std::printf("host_context_stress_tests: passed (%d ring switches)\n", kRounds * (kWorkers + 1));
    return 0;
}
