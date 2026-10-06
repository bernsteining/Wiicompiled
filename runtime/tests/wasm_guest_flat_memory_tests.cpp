// GuestFlat contract on WebAssembly (guest_flat_memory_wasm.cpp): no flat view, every access
// checked, and the physical/cached/uncached views of MEM1 and MEM2 aliasing one store.

#include "guest_flat_memory.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>

static_assert(!GuestFlat::kHasFlatView, "WebAssembly has no flat guest view");
static_assert(GuestFlat::RequiresCheckedAccess(), "WebAssembly guest accesses must be checked");

int main()
{
    const std::vector<GuestFlat::RegionRequest> layout = {
        {0x00000000u, 0x01800000u, GuestFlat::Backing::Mem1},
        {0x80000000u, 0x01800000u, GuestFlat::Backing::Mem1},
        {0xC0000000u, 0x01800000u, GuestFlat::Backing::Mem1},
        {0x10000000u, 0x08000000u, GuestFlat::Backing::Mem2},
        {0x90000000u, 0x08000000u, GuestFlat::Backing::Mem2},
        {0xD0000000u, 0x08000000u, GuestFlat::Backing::Mem2},
        {0x81800000u, 0x00200000u, GuestFlat::Backing::Owned},
        {0xE0000000u, 0x00101000u, GuestFlat::Backing::Owned},
    };
    GuestFlat::Initialize(layout);
    if (!GuestFlat::IsActive()) {
        std::cerr << "guest memory did not become active\n";
        return 1;
    }

    uint8_t* mem1[] = {GuestFlat::HostPointer(0x00000000u), GuestFlat::HostPointer(0x80000000u),
                       GuestFlat::HostPointer(0xC0000000u)};
    uint8_t* mem2[] = {GuestFlat::HostPointer(0x10000000u), GuestFlat::HostPointer(0x90000000u),
                       GuestFlat::HostPointer(0xD0000000u)};
    for (int i = 0; i < 3; ++i) {
        if (!mem1[i] || !mem2[i] || mem1[i] != mem1[0] || mem2[i] != mem2[0]) {
            std::cerr << "MEM1/MEM2 views do not alias one store\n";
            return 1;
        }
    }
    if (mem1[0] == mem2[0]) {
        std::cerr << "MEM1 and MEM2 share storage\n";
        return 1;
    }
    // Interior and last bytes resolve inside the same store; one past the end does not.
    if (GuestFlat::HostPointer(0x817FFFFFu) != mem1[0] + 0x017FFFFF ||
        GuestFlat::HostPointer(0xD7FFFFFFu) != mem2[0] + 0x07FFFFFF) {
        std::cerr << "interior addresses resolve to the wrong host byte\n";
        return 1;
    }
    uint8_t* overlay = GuestFlat::HostPointer(0x81800000u);
    uint8_t* lockedCache = GuestFlat::HostPointer(0xE0000000u);
    if (!overlay || !lockedCache || overlay == mem1[0] + 0x01800000) {
        std::cerr << "owned regions are missing or alias MEM1\n";
        return 1;
    }
    for (uint32_t unmapped : {0x01800000u, 0x18000000u, 0x81A00000u, 0xCC000000u, 0xE0101000u}) {
        if (GuestFlat::HostPointer(unmapped) != nullptr) {
            std::cerr << "unmapped guest address 0x" << std::hex << unmapped << " resolved\n";
            return 1;
        }
    }
    mem1[1][0x1234] = 0x5A;
    if (mem1[2][0x1234] != 0x5A || mem1[0][0x1234] != 0x5A || mem1[0][0x1235] != 0) {
        std::cerr << "MEM1 write not visible through every view, or memory not zeroed\n";
        return 1;
    }

    GuestFlat::Initialize(layout);  // the same layout again is accepted
    try {
        GuestFlat::Initialize({{0x80000000u, 0x01000000u, GuestFlat::Backing::Mem1}});
        std::cerr << "a different layout was accepted\n";
        return 1;
    } catch (const std::runtime_error&) {
    }
    if (GuestFlat::HandleAccessViolation(nullptr, false)) {
        std::cerr << "WebAssembly has no faults to resolve\n";
        return 1;
    }
    std::cout << "wasm_guest_flat_memory_tests: passed\n";
    return 0;
}
