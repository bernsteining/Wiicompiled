#pragma once
// Where the web page puts things in Emscripten's virtual file system before main() runs
// (runtime/web/loader.js and the --preload-file list in cmake/WebProduct.cmake).

#if defined(__EMSCRIPTEN__)
namespace WebMounts {
// The game's extracted DATA directory, read on demand through MKWDISCFS (disc_fs.js).
inline constexpr const char* kDisc = "/dvd";
// The browser's persistent storage (IDBFS): Config.toml, the NAND and saves, per application.
inline constexpr const char* kUserData = "/userdata";
// Runtime assets bundled with the build (preloaded): wii_bootstrap, dsp_coef.bin, cacert.pem.
inline constexpr const char* kAppAssets = "/app";
} // namespace WebMounts
#endif
