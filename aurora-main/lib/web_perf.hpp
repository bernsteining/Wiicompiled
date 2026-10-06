#pragma once
// Browser build only: CPU time spent in a block, added to the page's performance report
// (Module.mkwPerf.cpu, see the runtime's shell.html). Only the outermost scope of a bucket counts,
// so recursion (a display list calling back into the decoder) is not counted twice. Also event
// counters (Module.mkwPerf.counts).

#ifdef AURORA_WEBGPU_BROWSER
#include <emscripten/emscripten.h>

#include <cstdint>

// Defined in lib/aurora.cpp.
extern "C" void aurora_web_perf_add(int bucket, double milliseconds);
extern "C" void aurora_web_perf_count(int counter, double amount);

namespace aurora::web_perf {
enum Bucket : int { kGxDecode = 0, kEncode = 1, kAudio = 2, kBucketCount = 3 };

inline int g_depth[kBucketCount] = {};
inline uint32_t g_calls[kBucketCount] = {};
// Time one outermost scope in 2^shift and scale it up. Each reading is a call out to JavaScript
// (performance.now()); GX decode scopes run thousands of times a frame (one per display list), and
// timing every one made ~8,700 such calls per frame, measured in Chrome during a race.
inline constexpr int kSampleShift[kBucketCount] = {6, 0, 0};

// What a frame did, summed into Module.mkwPerf.counts for the page's report and its per-frame
// hitch lines (the names are in shell.html; the runtime's counters start at 32).
enum Counter : int {
  kPipelinesStarted = 0,  // asynchronous pipeline compiles started
  kPipelinesFinished = 1, // ... and finished
  kPipelineAsyncMs = 2,   // their latency, start to result, summed
  kPipelinesNow = 3,      // pipelines compiled synchronously (a draw could not skip, or no async path)
  kPipelineNowMs = 4,     // time spent in those
  kDrawsSkipped = 5,      // draws dropped because their pipeline was still compiling
  kDraws = 6,
  kUploadBytes = 7,       // per-frame vertex, index, uniform and storage data
  kTextureUploads = 8,
  kTextureBytes = 9,
  kBatches = 10,          // staging batches submitted (more than one a frame: capacity splits, EFB copies)
  kGpuFrames = 11,        // frames whose GPU work reported done
  kGpuMs = 12,            // their scene's submit-to-done time, summed (an upper bound on its GPU time)
  kUberDraws = 13,        // draws using the ubershader (hybrid: their specialised pipeline is not ready)
  kDeferredStarted = 14,  // deferred specialised pipelines started on a quiet frame
};

// A frame's scene was submitted at `submitted` (emscripten_get_now()) and the GPU reported it done
// `milliseconds` later: adds to kGpuFrames and kGpuMs and tracks the worst (Module.mkwPerf.gpuWorst).
extern "C" void aurora_web_perf_gpu_done(double milliseconds);

inline void count(Counter counter, double amount = 1.0) { aurora_web_perf_count(counter, amount); }

class Scope {
public:
  explicit Scope(Bucket bucket) noexcept : m_bucket(bucket) {
    const uint32_t sampleMask = (1u << kSampleShift[m_bucket]) - 1u;
    if (g_depth[m_bucket]++ == 0 && (g_calls[m_bucket]++ & sampleMask) == 0) m_started = emscripten_get_now();
  }
  ~Scope() {
    if (--g_depth[m_bucket] == 0 && m_started != 0.0) {
      aurora_web_perf_add(m_bucket, (emscripten_get_now() - m_started) * (1u << kSampleShift[m_bucket]));
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  Bucket m_bucket;
  double m_started = 0.0;
};
} // namespace aurora::web_perf
#endif
