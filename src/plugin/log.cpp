#include "log.h"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cstdio>
#include <memory>

namespace e2t::log {
namespace {

constexpr std::size_t kMaxMessageBytes = 1024;
constexpr std::size_t kMaxLineBytes = kMaxMessageBytes + 96;
constexpr char kSdkPrefix[] = "[ets2_trainer] ";
constexpr wchar_t kLogRelativePath[] = L"\\Euro Truck Simulator 2\\ets2_trainer_plugin.log";
constexpr std::int32_t kSdkLogMessage = 0;
constexpr std::int32_t kSdkLogWarning = 1;
constexpr std::int32_t kSdkLogError = 2;

SRWLOCK g_lock = SRWLOCK_INIT;
HANDLE g_file = INVALID_HANDLE_VALUE;
SdkLogFn g_sdkLog = nullptr;
DWORD g_sdkThreadId = 0;

class ExclusiveLock {
public:
    explicit ExclusiveLock(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~ExclusiveLock() { ReleaseSRWLockExclusive(&lock_); }
    ExclusiveLock(const ExclusiveLock&) = delete;
    ExclusiveLock& operator=(const ExclusiveLock&) = delete;

private:
    SRWLOCK& lock_;
};

const char* levelName(Level level) noexcept {
    switch (level) {
        case Level::Warning: return "WARN ";
        case Level::Error: return "ERROR";
        default: return "INFO ";
    }
}

std::int32_t sdkLevel(Level level) noexcept {
    switch (level) {
        case Level::Warning: return kSdkLogWarning;
        case Level::Error: return kSdkLogError;
        default: return kSdkLogMessage;
    }
}

void closeFileLocked() noexcept {
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

}  // namespace

void init(SdkLogFn sdkLog, const std::wstring& filePath) noexcept {
    ExclusiveLock guard(g_lock);
    closeFileLocked();
    g_sdkLog = sdkLog;
    g_sdkThreadId = GetCurrentThreadId();
    if (!filePath.empty()) {
        g_file = CreateFileW(filePath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
}

void shutdown() noexcept {
    ExclusiveLock guard(g_lock);
    closeFileLocked();
    g_sdkLog = nullptr;
    g_sdkThreadId = 0;
}

std::wstring defaultLogPath() noexcept {
    struct CoTaskMemDeleter {
        void operator()(wchar_t* memory) const noexcept { CoTaskMemFree(memory); }
    };
    try {
        PWSTR documents = nullptr;
        const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr,
                                                &documents);
        const std::unique_ptr<wchar_t, CoTaskMemDeleter> owner(documents);
        if (FAILED(hr) || documents == nullptr) {
            return std::wstring();
        }
        return std::wstring(documents) + kLogRelativePath;
    } catch (...) {
        return std::wstring();
    }
}

void writeV(Level level, const char* format, va_list args) noexcept {
    char message[kMaxMessageBytes];
    if (format == nullptr || std::vsnprintf(message, sizeof(message), format, args) < 0) {
        std::snprintf(message, sizeof(message), "(log format error)");
    }

    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[kMaxLineBytes];
    const int lineLength = std::snprintf(
        line, sizeof(line), "%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\r\n", now.wYear,
        now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
        levelName(level), message);

    ExclusiveLock guard(g_lock);
    if (g_file != INVALID_HANDLE_VALUE && lineLength > 0) {
        const DWORD bytes = static_cast<DWORD>(
            lineLength < static_cast<int>(sizeof(line)) ? lineLength : sizeof(line) - 1);
        DWORD written = 0;
        WriteFile(g_file, line, bytes, &written, nullptr);
    }
    if (g_sdkLog != nullptr && GetCurrentThreadId() == g_sdkThreadId) {
        char sdkLine[kMaxMessageBytes + sizeof(kSdkPrefix)];
        std::snprintf(sdkLine, sizeof(sdkLine), "%s%s", kSdkPrefix, message);
        g_sdkLog(sdkLevel(level), sdkLine);
    }
}

void write(Level level, const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    writeV(level, format, args);
    va_end(args);
}

void info(const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    writeV(Level::Info, format, args);
    va_end(args);
}

void warn(const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    writeV(Level::Warning, format, args);
    va_end(args);
}

void error(const char* format, ...) noexcept {
    va_list args;
    va_start(args, format);
    writeV(Level::Error, format, args);
    va_end(args);
}

}  // namespace e2t::log
