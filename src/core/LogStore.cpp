#include "core/LogStore.h"

#include <spdlog/sinks/base_sink.h>
#include <deque>
#include <mutex>
#include <algorithm>

namespace Nyx {

namespace {
std::mutex          g_mutex;
std::deque<LogLine> g_lines;
uint64_t            g_version = 0;
constexpr size_t    CAP = 1000;

// spdlog sink that forwards each record's level + message into the store.
template <typename Mutex>
class StoreSink : public spdlog::sinks::base_sink<Mutex> {
protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        LogStore::push(static_cast<int>(msg.level),
                       std::string(msg.payload.data(), msg.payload.size()));
    }
    void flush_() override {}
};
} // namespace

void LogStore::push(int level, std::string text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_lines.push_back({level, std::move(text)});
    while (g_lines.size() > CAP) g_lines.pop_front();
    ++g_version;
}

void LogStore::snapshot(std::vector<LogLine>& out, size_t maxLines) {
    std::lock_guard<std::mutex> lock(g_mutex);
    out.clear();
    size_t n = std::min(maxLines, g_lines.size());
    for (size_t i = g_lines.size() - n; i < g_lines.size(); ++i) out.push_back(g_lines[i]);
}

uint64_t LogStore::version() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_version;
}

size_t LogStore::count() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_lines.size();
}

void LogStore::attachSink(std::shared_ptr<spdlog::logger>& logger) {
    logger->sinks().push_back(std::make_shared<StoreSink<std::mutex>>());
}

} // namespace Nyx
