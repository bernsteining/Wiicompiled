#pragma once

// Web build only: event counts for the page's performance report (Module.mkwPerf.counts, named in
// runtime/web/shell.html). aurora counts its own from 0 (aurora-main/lib/web_perf.hpp); the
// runtime's start at 32. Elsewhere MKW_WEB_PERF_COUNT compiles to nothing.

#if defined(__EMSCRIPTEN__)
extern "C" void mkw_web_perf_count(int counter, double amount); // runtime/web/host_wait_web.cpp

namespace WebPerf {
enum Counter : int {
    kAudioBlocksDropped = 32, // the output queue was full
    kAudioQueueEmpty = 33,    // a block arrived to an empty output queue: playback had run dry
    kRetracesByDisplay = 34,  // VI retraces delivered on a display refresh (src/hle/vi.cpp)
    kRetracesByClock = 35,    // ... and on the wall-clock grid
};
} // namespace WebPerf

#define MKW_WEB_PERF_COUNT(counter, amount) mkw_web_perf_count(WebPerf::counter, (amount))
#else
#define MKW_WEB_PERF_COUNT(counter, amount) ((void)0)
#endif
