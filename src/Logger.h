#pragma once

// Logger.h — Engine-wide logging system
//
// Wraps the spdlog library to provide simple logging macros.
// Usage:
//   LOG_INFO("Engine started");
//   LOG_WARN("Frame took {}ms", deltaTime);
//   LOG_ERROR("Failed to create window: {}", errorMsg);

#include <spdlog/spdlog.h>
#include <memory>

namespace VulkanEngine {

class Logger {
public:
    // Call once at startup to initialize the logging system
    static void init();

    // Access the engine logger (used by the macros below)
    static std::shared_ptr<spdlog::logger>& getLogger();

private:
    static std::shared_ptr<spdlog::logger> s_logger;
};

} // namespace VulkanEngine

// ── Convenience macros ─────────────────────────────────────────────────────
// These let you write LOG_INFO("hello") instead of Logger::getLogger()->info("hello")
#define LOG_TRACE(...)  ::VulkanEngine::Logger::getLogger()->trace(__VA_ARGS__)
#define LOG_INFO(...)   ::VulkanEngine::Logger::getLogger()->info(__VA_ARGS__)
#define LOG_WARN(...)   ::VulkanEngine::Logger::getLogger()->warn(__VA_ARGS__)
#define LOG_ERROR(...)  ::VulkanEngine::Logger::getLogger()->error(__VA_ARGS__)
#define LOG_FATAL(...)  ::VulkanEngine::Logger::getLogger()->critical(__VA_ARGS__)
