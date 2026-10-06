// The web side of platform/host_wait.h.

#include <emscripten/em_js.h>

// A MessageChannel task: unlike setTimeout(0), it is not clamped to 4 ms when chained. One channel
// serves every call; a new one per call is garbage the collector has to chase.
EM_ASYNC_JS(void, mkw_web_yield_to_browser, (), {
    if (!Module['mkwYieldChannel']) {
        const channel = new MessageChannel();
        const waiting = [];
        channel.port1.onmessage = () => waiting.shift()();
        Module['mkwYieldChannel'] = (resolve) => {
            waiting.push(resolve);
            channel.port2.postMessage(0);
        };
    }
    const started = performance.now();
    await new Promise((resolve) => Module['mkwYieldChannel'](resolve));
    if (Module['mkwPerf']) Module['mkwPerf'].yield += performance.now() - started;
});

// emscripten_sleep's timer, timed for the page's performance report (Module.mkwPerf).
EM_ASYNC_JS(void, mkw_web_sleep, (unsigned int milliseconds), {
    const started = performance.now();
    await new Promise((resolve) => setTimeout(resolve, milliseconds));
    if (Module['mkwPerf']) Module['mkwPerf'].sleep += performance.now() - started;
});

// The display's refresh, which the VI pacing (src/hle/vi.cpp) uses as the retrace when the display
// runs at about the retrace rate. A requestAnimationFrame ticker, started by the first call to
// mkw_web_display_tick, counts refreshes and keeps the last 60 intervals between their timestamps.
// aurora's frame wait (aurora-main/lib/aurora.cpp) reads Module.mkwDisplay too: while `locked` is
// set it skips its own wait when a refresh has passed since the previous frame.
// ?novsync turns this pacing off (the retrace keeps its wall-clock grid), for comparison.
EM_JS(double, mkw_web_display_tick, (), {
    let display = Module['mkwDisplay'];
    if (!display) {
        if (new URLSearchParams(location.search).has('novsync')) return -1;
        display = Module['mkwDisplay'] = { count: 0, last: 0, intervals: [], waiting: [], locked: false };
        const tick = (time) => {
            if (display.last) {
                display.intervals.push(time - display.last);
                if (display.intervals.length > 60) display.intervals.shift();
            }
            display.last = time;
            display.count++;
            const waiting = display.waiting;
            display.waiting = [];
            for (const resolve of waiting) resolve();
            requestAnimationFrame(tick);
        };
        requestAnimationFrame(tick);
    }
    // A hidden page, or a main thread busy for a long stretch, gets no refreshes: report none.
    return display.last && performance.now() - display.last < 100 ? display.count : -1;
});

// The display's refresh period in milliseconds, or 0 until measured: the median of the last 60
// intervals. Not the shortest: on an iPad's 120 Hz ProMotion screen Safari runs animation frames at
// 60 Hz, but their timestamps fall on the 120 Hz grid, so some intervals are 8.3 ms (measured on an
// iPad Pro: the shortest made the pacing give up on most frames). When a busy main thread skips
// most refreshes the median goes up and the pacing rightly lets go.
EM_JS(double, mkw_web_display_period_ms, (), {
    const display = Module['mkwDisplay'];
    if (!display || display.intervals.length < 10) return 0;
    const sorted = display.intervals.slice().sort((a, b) => a - b);
    return sorted[sorted.length >> 1];
});

EM_JS(void, mkw_web_set_display_locked, (int locked), {
    if (Module['mkwDisplay']) Module['mkwDisplay'].locked = !!locked;
});

// Suspends until the next refresh, or for at most `timeoutMs` (the caller has timers to service).
EM_ASYNC_JS(void, mkw_web_wait_display_tick, (double timeoutMs), {
    const display = Module['mkwDisplay'];
    const started = performance.now();
    await new Promise((resolve) => {
        const timer = setTimeout(resolve, timeoutMs);
        display.waiting.push(() => { clearTimeout(timer); resolve(); });
    });
    if (Module['mkwPerf']) Module['mkwPerf'].frame += performance.now() - started;
});

// Event counts for the page's performance report (platform/web_perf.h).
EM_JS(void, mkw_web_perf_count, (int counter, double amount), {
    const perf = Module['mkwPerf'];
    if (perf && perf.counts) perf.counts[counter] += amount;
});
