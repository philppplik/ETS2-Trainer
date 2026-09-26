// Plugin log: game (SDK) log + a truncated-on-start file, timestamped, thread-safe.
#pragma once

#include <cstdarg>
#include <cstdint>
#include <string>

namespace e2t::log {

enum class Level { Info, Warning, Error };

// Same signature as the SDK log callback (scs::log_fn).
using SdkLogFn = void(__stdcall*)(std::int32_t type, const char* message);

// sdkLog may be null; filePath may be empty (file logging disabled). The SDK log is only
// called from the thread that called init() (the game thread).
void init(SdkLogFn sdkLog, const std::wstring& filePath) noexcept;
void shutdown() noexcept;

// %USERPROFILE%\Documents\Euro Truck Simulator 2\ets2_trainer_plugin.log (empty on failure).
std::wstring defaultLogPath() noexcept;

void write(Level level, const char* format, ...) noexcept;
void writeV(Level level, const char* format, va_list args) noexcept;

void info(const char* format, ...) noexcept;
void warn(const char* format, ...) noexcept;
void error(const char* format, ...) noexcept;

}  // namespace e2t::log
