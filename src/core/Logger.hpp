#pragma once

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

namespace Meridian {

class Logger {
public:
    static void init()
    {
        s_logger = spdlog::stdout_color_mt("Meridian");
        s_logger->set_pattern("[%T] [%^%l%$] %n: %v");
        s_logger->set_level(spdlog::level::trace);
    }

    [[nodiscard]] static std::shared_ptr<spdlog::logger>& get() noexcept
    {
        return s_logger;
    }

private:
    static inline std::shared_ptr<spdlog::logger> s_logger; // NOLINT
};

} // namespace Meridian

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define MRD_TRACE(...)    ::Meridian::Logger::get()->trace(__VA_ARGS__)
#define MRD_INFO(...)     ::Meridian::Logger::get()->info(__VA_ARGS__)
#define MRD_WARN(...)     ::Meridian::Logger::get()->warn(__VA_ARGS__)
#define MRD_ERROR(...)    ::Meridian::Logger::get()->error(__VA_ARGS__)
#define MRD_CRITICAL(...) ::Meridian::Logger::get()->critical(__VA_ARGS__)
// NOLINTEND(cppcoreguidelines-macro-usage)
