// Steam Cloud save writer (see cloud_storage.h).
#include "cloud_storage.h"

#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <fstream>
#include <string>
#include <vector>

#include "log.h"
#include "../../shared/bridge_protocol.h"

namespace e2t::cloud {
namespace {

constexpr std::size_t kMaxFileBytes = 128u << 20;  // saves are ~2-30 MB
constexpr std::size_t kMaxRemoteName = 240;
constexpr std::size_t kMaxRequestLines = 16;

using GetStorageFn = void*(__cdecl*)();
using FileWriteFn = bool(__cdecl*)(void* self, const char* name, const void* data, std::int32_t size);
using GetQuotaFn = bool(__cdecl*)(void* self, std::uint64_t* total, std::uint64_t* available);

struct SteamApi {
    GetStorageFn storage = nullptr;
    FileWriteFn fileWrite = nullptr;
    GetQuotaFn getQuota = nullptr;
};

SteamApi resolveSteam() noexcept {
    SteamApi api;
    HMODULE module = GetModuleHandleW(L"steam_api64.dll");
    if (module == nullptr) {
        return api;
    }
    api.storage = reinterpret_cast<GetStorageFn>(GetProcAddress(module, "SteamAPI_SteamRemoteStorage_v016"));
    if (api.storage == nullptr) {
        api.storage = reinterpret_cast<GetStorageFn>(GetProcAddress(module, "SteamAPI_SteamRemoteStorage_v014"));
    }
    api.fileWrite = reinterpret_cast<FileWriteFn>(
        GetProcAddress(module, "SteamAPI_ISteamRemoteStorage_FileWrite"));
    api.getQuota = reinterpret_cast<GetQuotaFn>(
        GetProcAddress(module, "SteamAPI_ISteamRemoteStorage_GetQuota"));
    return api;
}

void* storageInterface(const SteamApi& api) noexcept {
    if (api.storage == nullptr || api.fileWrite == nullptr) {
        return nullptr;
    }
    __try {
        return api.storage();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool guardedWrite(const SteamApi& api, void* storage, const char* name, const void* data,
                  std::int32_t size) noexcept {
    __try {
        return api.fileWrite(storage, name, data, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring localAppData() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        result = path;
    }
    CoTaskMemFree(path);
    return result;
}

std::wstring widen(const std::string& utf8) {
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, out.data(), needed);
    return out;
}

struct Entry {
    std::string remote;
    std::wstring local;
};

bool readFile(const std::wstring& path, std::vector<char>& out) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size <= 0 || static_cast<std::size_t>(size) > kMaxFileBytes) {
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    file.seekg(0);
    return static_cast<bool>(file.read(out.data(), size));
}

WriteResult fail(std::int32_t code, const char* message) noexcept {
    WriteResult result;
    result.code = code;
    std::snprintf(result.message, sizeof(result.message), "%s", message);
    log::warn("cloud save: %s", message);
    return result;
}

bool parseRequest(const std::wstring& stage, std::vector<Entry>& entries) {
    std::ifstream request(stage + L"\\..\\cloud_request.txt", std::ios::binary);
    if (!request) {
        return false;
    }
    std::string line;
    while (std::getline(request, line) && entries.size() < kMaxRequestLines) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t tab = line.find('\t');
        if (line.empty() || tab == std::string::npos) {
            continue;
        }
        Entry entry{line.substr(0, tab), widen(line.substr(tab + 1))};
        if (!isValidRemoteName(entry.remote.c_str()) || !isInsideStage(entry.local.c_str(), stage.c_str())) {
            return false;
        }
        entries.push_back(std::move(entry));
    }
    return !entries.empty();
}

}  // namespace

bool isValidRemoteName(const char* name) noexcept {
    if (name == nullptr) {
        return false;
    }
    const std::size_t length = std::strlen(name);
    if (length < 12 || length > kMaxRemoteName || std::strncmp(name, "profiles/", 9) != 0) {
        return false;
    }
    if (std::strstr(name, "/save/") == nullptr || std::strstr(name, "..") != nullptr ||
        std::strchr(name, '\\') != nullptr || std::strchr(name, ':') != nullptr) {
        return false;
    }
    return length > 4 && std::strcmp(name + length - 4, ".sii") == 0;
}

bool isInsideStage(const wchar_t* path, const wchar_t* stageDirectory) noexcept {
    if (path == nullptr || stageDirectory == nullptr || std::wcsstr(path, L"..") != nullptr) {
        return false;
    }
    const std::size_t prefix = std::wcslen(stageDirectory);
    return prefix > 0 && _wcsnicmp(path, stageDirectory, prefix) == 0 &&
           (path[prefix] == L'\\' || path[prefix] == L'/');
}

bool available() noexcept { return storageInterface(resolveSteam()) != nullptr; }

WriteResult writeRequestedFiles() noexcept {
    try {
        const SteamApi api = resolveSteam();
        void* storage = storageInterface(api);
        if (storage == nullptr) {
            return fail(kSaveWriteNoCloud, "Steam Cloud nicht erreichbar");
        }
        const std::wstring stage = localAppData() + L"\\ETS2Trainer\\stage";
        std::vector<Entry> entries;
        if (!parseRequest(stage, entries)) {
            return fail(kSaveWriteBadRequest, "Speicher-Auftrag fehlt oder ist ungültig");
        }
        std::vector<std::vector<char>> data(entries.size());
        std::uint64_t totalBytes = 0;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!readFile(entries[i].local, data[i])) {
                return fail(kSaveWriteReadFailed, "vorbereitete Save-Datei nicht lesbar");
            }
            totalBytes += data[i].size();
        }
        std::uint64_t quotaTotal = 0;
        std::uint64_t quotaFree = 0;
        if (api.getQuota != nullptr && api.getQuota(storage, &quotaTotal, &quotaFree) &&
            quotaTotal > 0 && quotaFree < totalBytes) {
            return fail(kSaveWriteQuota, "zu wenig Steam-Cloud-Speicher – alte Spielstände löschen");
        }
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (!guardedWrite(api, storage, entries[i].remote.c_str(), data[i].data(),
                              static_cast<std::int32_t>(data[i].size()))) {
                return fail(kSaveWriteRejected, "Steam hat das Schreiben abgelehnt");
            }
            log::info("cloud save: wrote %s (%zu bytes)", entries[i].remote.c_str(), data[i].size());
        }
        WriteResult result;
        result.code = static_cast<std::int32_t>(entries.size());
        std::snprintf(result.message, sizeof(result.message),
                      "Spielstand in die Steam Cloud geschrieben (%zu Dateien) – jetzt im Spiel laden",
                      entries.size());
        return result;
    } catch (...) {
        return fail(kSaveWriteRejected, "unerwarteter Fehler beim Schreiben");
    }
}

}  // namespace e2t::cloud
