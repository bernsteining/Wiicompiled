// The web build's DATA directory (runtime/web/disc_fs.js) seen through the runtime's
// own code paths: std::filesystem discovery, DvdReadContract::ReadExact (JSPI-suspending reads),
// and std::ifstream for files the page fetched up front. web_disc_fs_tests.pre.js mounts the disc.

#include "hle/dvd_contract.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

void Require(bool condition, const char* what)
{
    if (!condition) {
        std::fprintf(stderr, "web_disc_fs_tests: %s\n", what);
        std::exit(1);
    }
}

uint8_t Expected(uint32_t file, uint32_t index)
{
    return static_cast<uint8_t>((static_cast<uint32_t>(index * 2654435761u) >> 24) ^ (file * 31u));
}

void RequireRange(const char* path, uint32_t file, uint64_t offset, uint32_t length)
{
    std::vector<uint8_t> data;
    DvdReadContract::HostReadFailure failure;
    Require(DvdReadContract::ReadExact(path, offset, length, data, failure), "ReadExact failed on a valid range");
    Require(failure == DvdReadContract::HostReadFailure::None && data.size() == length, "ReadExact size");
    for (uint32_t i = 0; i < length; ++i) {
        if (data[i] != Expected(file, static_cast<uint32_t>(offset) + i)) {
            Require(false, "ReadExact returned the wrong bytes");
        }
    }
}

void RequireFailure(const char* path, uint64_t offset, uint32_t length, DvdReadContract::HostReadFailure expected)
{
    std::vector<uint8_t> data{1, 2, 3};
    DvdReadContract::HostReadFailure failure;
    Require(!DvdReadContract::ReadExact(path, offset, length, data, failure), "ReadExact accepted a bad read");
    Require(failure == expected, "ReadExact reported the wrong failure");
    Require(data.size() == 3, "a failed ReadExact must leave the destination untouched");
}

} // namespace

int main()
{
    std::map<std::string, uintmax_t> sizes;
    for (const auto& entry : fs::recursive_directory_iterator("/dvd")) {
        if (entry.is_regular_file()) {
            sizes[entry.path().generic_string()] = entry.file_size();
        }
    }
    const std::map<std::string, uintmax_t> expectedSizes{
        {"/dvd/files/a.bin", 300000},
        {"/dvd/files/dir/b.bin", 1234},
        {"/dvd/files/empty.bin", 0},
        {"/dvd/sys/fst.bin", 64},
    };
    Require(sizes == expectedSizes, "directory walk did not see the mounted files and sizes");
    Require(fs::is_directory("/dvd/files/dir") && fs::is_directory("/dvd/sys"), "directories missing");

    RequireRange("/dvd/files/a.bin", 1, 0, 1);
    RequireRange("/dvd/files/a.bin", 1, 0, 300000);
    RequireRange("/dvd/files/a.bin", 1, 123457, 4096);
    RequireRange("/dvd/files/a.bin", 1, 299999, 1);
    RequireRange("/dvd/files/dir/b.bin", 2, 0, 1234);
    RequireRange("/dvd/sys/fst.bin", 4, 8, 32);

    // Same failure semantics as the native ifstream path.
    using Failure = DvdReadContract::HostReadFailure;
    RequireFailure("/dvd/files/missing.bin", 0, 1, Failure::MissingFile);
    RequireFailure("/dvd/files/a.bin", 300000, 1, Failure::BadOffset);
    RequireFailure("/dvd/files/a.bin", 299999, 2, Failure::ShortRead);
    RequireFailure("/dvd/files/empty.bin", 0, 0, Failure::BadOffset);
    RequireFailure("/dvd/files/a.bin", 0x100000000ull, 1, Failure::BadOffset);

    // A file fetched up front is readable synchronously, as LoadFstIndex does.
    {
        std::ifstream fst("/dvd/sys/fst.bin", std::ios::binary);
        std::vector<char> data(64);
        Require(fst.read(data.data(), 64) && fst.gcount() == 64, "sync read of an up-front file");
        for (uint32_t i = 0; i < 64; ++i) {
            Require(static_cast<uint8_t>(data[i]) == Expected(4, i), "sync read returned the wrong bytes");
        }
    }
    // A large file fails a synchronous read cleanly instead of blocking the page.
    {
        std::ifstream large("/dvd/files/a.bin", std::ios::binary);
        char byte = 0;
        Require(!large.read(&byte, 1), "a synchronous read of an async-only file must fail");
    }
    // The mount is read-only.
    {
        std::ofstream created("/dvd/files/new.bin");
        Require(!created.is_open(), "the disc mount accepted a new file");
    }

    std::printf("web_disc_fs_tests: passed\n");
    return 0;
}
