#include "guest_flat_memory.h"

// WebAssembly guest memory. There is no fixed-address reservation, no page protection and no
// fault handler, so this backend only provides host storage and the aliasing between Wii views:
// the physical, cached and uncached views of MEM1 (and of MEM2) resolve to one store, exactly as
// the shared sections do on the other hosts. RequiresCheckedAccess() is constant-true on this
// host, so MMIO, deferred EFB reads and executable-write guarding all go through the checked
// Memory::* path instead of page traps, and the protection hooks below have nothing to do.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace GuestFlat {
namespace {

struct Store {
    Backing backing = Backing::Owned;
    uint32_t ownedBase = 0;  // only meaningful for Backing::Owned
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

std::mutex g_mutex;
std::vector<Store> g_stores;
std::vector<Mapping> g_mappings;
std::vector<RegionRequest> g_layout;
bool g_active = false;

// Offset of a view inside its backing store: every MEM1 view starts at physical 0 and every MEM2
// view at physical 0x10000000.
uint64_t StoreOffset(const RegionRequest& region)
{
    if (region.backing == Backing::Mem1) return region.base & 0x1FFFFFFFu;
    if (region.backing == Backing::Mem2) return (region.base & 0x1FFFFFFFu) - 0x10000000u;
    return 0;
}

uint32_t OwnedKey(const RegionRequest& region)
{
    return region.backing == Backing::Owned ? region.base : 0u;
}

bool SameLayout(const std::vector<RegionRequest>& a, const std::vector<RegionRequest>& b)
{
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
               return x.base == y.base && x.size == y.size && x.backing == y.backing;
           });
}

} // namespace

bool IsActive()
{
    return g_active;
}

void Initialize(const std::vector<RegionRequest>& regions)
{
    std::lock_guard lock(g_mutex);
    if (g_active) {
        if (!SameLayout(g_layout, regions)) {
            throw std::runtime_error("flat guest layout cannot be remapped");
        }
        return;
    }

    for (const auto& region : regions) {
        if (region.size == 0) continue;
        if (region.backing != Backing::Owned && StoreOffset(region) > 0x1FFFFFFFu) {
            throw std::runtime_error("guest region lies outside its MEM1/MEM2 backing");
        }
        const uint64_t needed = StoreOffset(region) + region.size;
        auto store = std::find_if(g_stores.begin(), g_stores.end(), [&](const Store& s) {
            return s.backing == region.backing && s.ownedBase == OwnedKey(region);
        });
        if (store == g_stores.end()) {
            g_stores.push_back(Store{region.backing, OwnedKey(region), needed, nullptr});
        } else {
            store->size = std::max(store->size, needed);
        }
    }

    for (auto& store : g_stores) {
        // Zeroed like fresh anonymous mappings; 4 KiB alignment keeps every Wii page aligned.
        store.host = static_cast<uint8_t*>(std::aligned_alloc(kGuestPageSize,
            (store.size + kGuestPageSize - 1) & ~static_cast<uint64_t>(kGuestPageSize - 1)));
        if (!store.host) {
            throw std::runtime_error("unable to allocate WebAssembly guest memory");
        }
        std::memset(store.host, 0, store.size);
    }

    for (const auto& region : regions) {
        if (region.size == 0) continue;
        const auto& store = *std::find_if(g_stores.begin(), g_stores.end(), [&](const Store& s) {
            return s.backing == region.backing && s.ownedBase == OwnedKey(region);
        });
        g_mappings.push_back(Mapping{region.base, region.size, store.host + StoreOffset(region)});
    }
    g_layout = regions;
    g_active = true;
}

uint8_t* HostPointer(uint32_t guestAddress)
{
    for (const auto& mapping : g_mappings) {
        if (guestAddress >= mapping.base && uint64_t{guestAddress - mapping.base} < mapping.size) {
            return mapping.host + (guestAddress - mapping.base);
        }
    }
    return nullptr;
}

void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }

} // namespace GuestFlat
