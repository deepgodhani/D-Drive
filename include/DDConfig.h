#pragma once

#include <cstddef>

namespace dd {
    // Tune these safely; start conservative, then increase after testing.
    inline constexpr std::size_t DEFAULT_CHUNK_SIZE = 128ull * 1024ull * 1024ull; // 128 MB per chunk
    inline constexpr int MAX_INFLIGHT_UPLOADS = 3;        // limit memory while using big chunks
    inline constexpr int MAX_RETRIES = 5;
    inline constexpr int BASE_BACKOFF_MS = 500;           // 0.5s → 8s
}
