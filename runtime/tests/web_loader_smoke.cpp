// Browser smoke test for the web page loader (runtime/web/loader.js + disc_fs.js + IDBFS):
// prints every file under /dvd with its size and an FNV-1a hash of its bytes read through
// DvdReadContract::ReadExact, hashes sys/fst.bin through std::ifstream, and bumps a counter in
// /userdata to show it persists across reloads. Serve with serve.py --data <DATA dir> and open
// mkw_web_loader_smoke.html?data=data.

#include "hle/dvd_contract.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
uint64_t Fnv1a(uint64_t hash, const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    return hash;
}
} // namespace

int main()
{
    bool ok = true;
    for (const auto& entry : fs::recursive_directory_iterator("/dvd")) {
        if (!entry.is_regular_file()) continue;
        const uint64_t size = entry.file_size();
        uint64_t hash = 14695981039346656037ull;
        for (uint64_t offset = 0; offset < size; offset += 65536) {
            const uint32_t length = static_cast<uint32_t>(std::min<uint64_t>(65536, size - offset));
            std::vector<uint8_t> data;
            DvdReadContract::HostReadFailure failure;
            if (!DvdReadContract::ReadExact(entry.path(), offset, length, data, failure)) {
                std::printf("SMOKE read-failed %s %s\n", entry.path().c_str(), DvdReadContract::Describe(failure));
                ok = false;
                break;
            }
            hash = Fnv1a(hash, data.data(), data.size());
        }
        std::printf("SMOKE file %s %llu %016llx\n", entry.path().generic_string().c_str() + 5,
                    static_cast<unsigned long long>(size), static_cast<unsigned long long>(hash));
    }

    {
        std::ifstream fst("/dvd/sys/fst.bin", std::ios::binary);
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(fst)), std::istreambuf_iterator<char>());
        std::printf("SMOKE ifstream sys/fst.bin %zu %016llx\n", data.size(),
                    static_cast<unsigned long long>(Fnv1a(14695981039346656037ull, data.data(), data.size())));
    }

    int runs = 0;
    {
        std::ifstream counter("/userdata/smoke_runs.txt");
        counter >> runs;
    }
    {
        std::ofstream counter("/userdata/smoke_runs.txt", std::ios::trunc);
        counter << runs + 1;
    }
    std::printf("SMOKE runs %d\n", runs + 1);
    std::printf("SMOKE %s\n", ok ? "done" : "failed");
    return 0;
}
