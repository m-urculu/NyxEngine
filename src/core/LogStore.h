#pragma once

// LogStore.h — In-memory ring buffer of recent log lines, fed by a spdlog sink.
// The on-screen Console reads from it. Decouples log capture from the UI.

#include <spdlog/spdlog.h>
#include <string>
#include <vector>
#include <cstdint>

namespace Nyx {

struct LogLine {
    int         level;   // spdlog level (0 trace … 5 critical)
    std::string text;    // the message payload
};

class LogStore {
public:
    // Attach a capturing sink to the given logger (call once at init).
    static void attachSink(std::shared_ptr<spdlog::logger>& logger);

    static void push(int level, std::string text);

    // Copy the most recent `maxLines` lines (oldest→newest) into `out`.
    static void snapshot(std::vector<LogLine>& out, size_t maxLines);

    // Number of lines currently stored.
    static size_t count();

    // Monotonic counter that bumps on every push — lets the UI skip rebuilds.
    static uint64_t version();
};

} // namespace Nyx
