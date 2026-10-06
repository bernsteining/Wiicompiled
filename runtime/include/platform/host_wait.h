#pragma once

// Waiting on the host. Native builds block the calling thread, exactly as std::this_thread does.
// The web build runs everything on the browser's main thread, which must never block: there these
// suspend the calling stack through JSPI, so rendering, input, audio and timer callbacks keep
// running during the wait. Whole milliseconds are waited on a browser timer; what is left, and any
// wait under a millisecond, goes to event-loop turns (MessageChannel tasks), because browsers clamp
// chained timeouts to ~4 ms.

#include <chrono>
#include <thread>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>

// One turn of the browser's event loop, a timer wait, and the display's refreshes (count, -1 while
// none arrive; period in ms, 0 until measured; a wait for the next one); runtime/web/host_wait_web.cpp.
// EM_ASYNC_JS functions are imported as "__asyncjs__<name>"; the declarations carry that name so a
// call compiled before the definition is seen (unity builds) still binds to the right import.
extern "C" void mkw_web_yield_to_browser() EM_IMPORT(__asyncjs__mkw_web_yield_to_browser);
extern "C" void mkw_web_sleep(unsigned int milliseconds) EM_IMPORT(__asyncjs__mkw_web_sleep);
extern "C" double mkw_web_display_tick();
extern "C" double mkw_web_display_period_ms();
extern "C" void mkw_web_set_display_locked(int locked);
extern "C" void mkw_web_wait_display_tick(double timeoutMs) EM_IMPORT(__asyncjs__mkw_web_wait_display_tick);
#endif

namespace HostWait {

template <class Rep, class Period>
inline void SleepFor(std::chrono::duration<Rep, Period> duration)
{
#if defined(__EMSCRIPTEN__)
    const auto milliseconds = std::chrono::floor<std::chrono::milliseconds>(duration).count();
    if (milliseconds >= 1) {
        mkw_web_sleep(static_cast<unsigned int>(milliseconds));
    } else {
        mkw_web_yield_to_browser();
    }
#else
    std::this_thread::sleep_for(duration);
#endif
}

template <class Clock, class Duration>
inline void SleepUntil(std::chrono::time_point<Clock, Duration> deadline)
{
#if defined(__EMSCRIPTEN__)
    for (auto now = Clock::now(); now < deadline; now = Clock::now()) {
        SleepFor(deadline - now);
    }
#else
    std::this_thread::sleep_until(deadline);
#endif
}

} // namespace HostWait
