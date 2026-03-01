#include "Logger.h"
#include <spdlog/sinks/stdout_color_sinks.h>

namespace VulkanEngine {

// Static member — lives for the entire program lifetime
std::shared_ptr<spdlog::logger> Logger::s_logger;

void Logger::init() {
    // Create a colored console logger named "ENGINE"
    // Output format: [HH:MM:SS] [ENGINE] [level] message
    spdlog::set_pattern("[%T] [%n] [%^%l%$] %v");
    s_logger = spdlog::stdout_color_mt("ENGINE");
    s_logger->set_level(spdlog::level::trace);
    s_logger->info("Logger initialized");
}

std::shared_ptr<spdlog::logger>& Logger::getLogger() {
    return s_logger;
}

} // namespace VulkanEngine
