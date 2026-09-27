// Steam Cloud save writer: writes staged save files through the game's own Steam Remote Storage.
//
// Steam-Cloud profiles are read by ETS2 through ISteamRemoteStorage ("/steam/profiles/..."), so
// files that another program drops into Steam's userdata folder stay invisible in the load menu.
// Running inside the game process, the plugin can call the same API (flat exports of
// steam_api64.dll) and the saves appear immediately. The app stages the files and lists them in
// %LOCALAPPDATA%\ETS2Trainer\cloud_request.txt as "remote_name<TAB>local_path" lines.
#pragma once

#include <cstddef>
#include <cstdint>

namespace e2t::cloud {

struct WriteResult {
    std::int32_t code = 0;  // files written (>= 0) or SaveWriteError
    char message[192] = {};
};

// True when steam_api64.dll is loaded and SteamRemoteStorage() answers.
bool available() noexcept;

// Reads the request file and writes every listed file. Call on the game thread.
WriteResult writeRequestedFiles() noexcept;

// Validation helpers (exposed for the self-test).
bool isValidRemoteName(const char* name) noexcept;
bool isInsideStage(const wchar_t* path, const wchar_t* stageDirectory) noexcept;

}  // namespace e2t::cloud
