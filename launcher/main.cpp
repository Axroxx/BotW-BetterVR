#include "pch.h"

#include <array>
#include <cmath>
#include <ktmw32.h>

#include "embedded_assets.h"
#include "launcher_common.h"

static constexpr wchar_t VulkanImplicitLayersRegistryPath[] = L"SOFTWARE\\Khronos\\Vulkan\\ImplicitLayers";
static constexpr std::string_view BundledGraphicPackName = "BreathOfTheWild_BetterVR";
static constexpr std::string_view BundledGraphicPackPrefix = "BreathOfTheWild_BetterVR/";
static constexpr std::string_view BundledDownloadedGraphicsPrefix = "BreathOfTheWild_Graphics/";
static constexpr uint32_t BetterVRResolutionSearchAlignment = 8;
static constexpr uint32_t BetterVRResolutionSearchRadius = 32;
static constexpr double BetterVRPreferredMatchDistance = 32.0;
static constexpr std::wstring_view LegacyLaunchFiles[] = {
    L"Launch_BetterVR.bat",
    L"BetterVR LAUNCH CEMU IN VR.bat",
    L"BetterVR LAUNCH CEMU IN VR - COMPATIBILITY MODE.bat",
    L"BetterVR UNINSTALL.bat",
    L"BetterVR_Layer.dll",
    L"VkLayer_BetterVR_Hook.dll",
    L"BetterVR.txt",
};

struct EmbeddedAssetView {
    const void* data = nullptr;
    size_t size = 0;
};

struct BetterVRTextureRedefineRule {
    uint32_t width;
    uint32_t height;
    double weight;
    double heightBias;
};

static constexpr std::array<BetterVRTextureRedefineRule, 12> BetterVRTextureRedefineRules = {{
    { 640, 368, 8.0, 0.49 },
    { 640, 360, 8.0, 0.0 },
    { 384, 192, 7.0, 0.0 },
    { 320, 192, 7.0, 0.0 },
    { 320, 180, 7.0, 0.0 },
    { 192, 96, 6.0, 0.0 },
    { 160, 96, 6.0, 0.0 },
    { 160, 90, 6.0, 0.0 },
    { 128, 48, 5.0, 0.0 },
    { 96, 48, 5.0, 0.0 },
    { 80, 45, 4.0, 0.0 },
    { 64, 64, 4.0, 0.0 },
}};

struct TemporaryDownloadedGraphicsFiles {
    std::vector<fs::path> paths;

    void DeleteAll() {
        for (const fs::path& path : paths) {
            std::error_code ec;
            fs::remove(path, ec);
            if (ec && ec.value() != ERROR_FILE_NOT_FOUND) {
                LogLine("Failed to remove temporary downloaded graphics override file: " + Narrow(path) + " (" + ec.message() + ")");
            }
        }
        paths.clear();
    }

    ~TemporaryDownloadedGraphicsFiles() {
        DeleteAll();
    }
};

static std::optional<EmbeddedAssetView> LoadEmbeddedAsset(const EmbeddedAssets::Asset& asset) {
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(asset.resourceId), MAKEINTRESOURCEW(10));
    if (resource == nullptr) {
        LogLine("Failed to find embedded resource: " + std::string(asset.relativePath));
        return std::nullopt;
    }

    HGLOBAL resourceHandle = LoadResource(nullptr, resource);
    if (resourceHandle == nullptr) {
        LogLine("Failed to load embedded resource: " + std::string(asset.relativePath));
        return std::nullopt;
    }

    const DWORD resourceSize = SizeofResource(nullptr, resource);
    const void* resourceData = LockResource(resourceHandle);
    if (resourceData == nullptr && resourceSize != 0) {
        LogLine("Failed to lock embedded resource: " + std::string(asset.relativePath));
        return std::nullopt;
    }

    return EmbeddedAssetView{ resourceData, static_cast<size_t>(resourceSize) };
}

static std::string GetActiveOpenXRRuntimePath() {
    auto getKey = [](HKEY rootKey, const char* subKey) -> std::string {
        HKEY key = nullptr;
        if (RegOpenKeyExA(rootKey, subKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
            return {};
        }

        char value[MAX_PATH] = {};
        DWORD size = sizeof(value);
        const LONG queryResult = RegQueryValueExA(key, "ActiveRuntime", nullptr, nullptr, reinterpret_cast<LPBYTE>(value), &size);
        RegCloseKey(key);
        if (queryResult != ERROR_SUCCESS) {
            return {};
        }

        return std::string(value);
    };

    std::string path = getKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1");
    if (path.empty()) {
        path = getKey(HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Khronos\\OpenXR\\1");
    }
    return path;
}

static bool IsSteamVRRunning() {
    HKEY key = nullptr;
    DWORD running = 0;
    DWORD size = sizeof(running);
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\Valve\\Steam\\Apps\\250820", 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }

    const LONG queryResult = RegQueryValueExA(key, "Running", nullptr, nullptr, reinterpret_cast<LPBYTE>(&running), &size);
    RegCloseKey(key);
    return queryResult == ERROR_SUCCESS && running == 1;
}

static bool EnsureOpenXRRuntimeReady() {
    char envBuffer[MAX_PATH] = {};
    const DWORD envResult = GetEnvironmentVariableA("XR_RUNTIME_JSON", envBuffer, sizeof(envBuffer));
    if (envResult > 0 && envResult < sizeof(envBuffer)) {
        LogLine(std::string("OpenXR runtime overridden by XR_RUNTIME_JSON: ") + envBuffer);
        return true;
    }

    const std::string runtimePath = GetActiveOpenXRRuntimePath();
    if (runtimePath.empty()) {
        ShowErrorMessage("No OpenXR runtime found.\n\nPlease set an OpenXR runtime using https://academy.vrex.no/knowledge-base/openxr/\n\nVirtual Desktop can also be used with its included VDXR runtime for better performance.");
        return false;
    }

    LogLine("Active OpenXR runtime path: " + runtimePath);

    std::string runtimeLower = runtimePath;
    std::ranges::transform(runtimeLower, runtimeLower.begin(), [](unsigned char character) {
        return char(std::tolower(character));
    });

    if (runtimeLower.find("steamvr") != std::string::npos && !IsSteamVRRunning()) {
        const int messageBoxId = MessageBoxA(nullptr, "SteamVR is set as the active runtime but does not appear to be running.\n\nDo you want to launch SteamVR?", "Launch SteamVR?", MB_YESNO | MB_ICONQUESTION);
        if (messageBoxId == IDYES) {
            LogLine("Launching SteamVR through steam://run/250820");
            ShellExecuteA(nullptr, "open", "steam://run/250820", nullptr, nullptr, SW_SHOW);
        }
        else {
            LogLine("SteamVR launch prompt declined");
        }
    }

    return true;
}

static bool ExtractRuntimeFiles(const LauncherPaths& paths) {
    if (!EnsureDirectory(paths.targetPack, "Failed to create runtime directory")) {
        return false;
    }

    for (size_t index = 0; index < EmbeddedAssets::AssetCount; ++index) {
        const EmbeddedAssets::Asset& asset = EmbeddedAssets::Assets[index];
        if (asset.kind != EmbeddedAssets::AssetKind::Runtime) {
            continue;
        }

        const std::optional<EmbeddedAssetView> assetView = LoadEmbeddedAsset(asset);
        if (!assetView.has_value()) {
            return false;
        }

        const fs::path outputPath = paths.targetPack / fs::path(asset.relativePath);
        if (!WriteBinaryFile(outputPath, static_cast<const unsigned char*>(assetView->data), assetView->size)) {
            return false;
        }

        LogLine("Extracted runtime file: " + Narrow(outputPath));
    }

    return true;
}

static bool InstallGraphicPack(const LauncherPaths& paths) {
    if (!EnsureDirectory(paths.targetBase, "Failed to create graphic packs directory")) {
        return false;
    }

    std::error_code ec;
    fs::remove_all(paths.targetPack, ec);
    if (ec) {
        LogLine("Failed to remove old BetterVR graphic pack: " + ec.message());
        return false;
    }

    if (!EnsureDirectory(paths.targetPack, "Failed to create BetterVR graphic pack directory")) {
        return false;
    }

    for (size_t index = 0; index < EmbeddedAssets::AssetCount; ++index) {
        const EmbeddedAssets::Asset& asset = EmbeddedAssets::Assets[index];
        if (asset.kind != EmbeddedAssets::AssetKind::GraphicPack) {
            continue;
        }

        const std::optional<EmbeddedAssetView> assetView = LoadEmbeddedAsset(asset);
        if (!assetView.has_value()) {
            return false;
        }

        std::string_view relativePath = asset.relativePath;
        if (!relativePath.starts_with(BundledGraphicPackPrefix)) {
            LogLine("Embedded asset had unexpected graphic pack path: " + std::string(relativePath));
            return false;
        }

        relativePath.remove_prefix(BundledGraphicPackPrefix.size());
        const fs::path outputPath = paths.targetPack / fs::path(std::string(relativePath));
        if (!WriteBinaryFile(outputPath, static_cast<const unsigned char*>(assetView->data), assetView->size)) {
            return false;
        }
    }

    LogLine("Installed BetterVR graphic pack to: " + Narrow(paths.targetPack));
    return true;
}

static std::string ToLower(std::string value) {
    std::ranges::transform(value, value.begin(), [](unsigned char character) {
        return char(std::tolower(character));
    });
    return value;
}

static bool IsBetterVRRegistryValue(const std::wstring& valueName) {
    const std::string lowered = ToLower(Narrow(valueName));
    return lowered.find("bettervr") != std::string::npos && lowered.ends_with("bettervr_layer.json");
}

static bool RemoveBetterVREntriesFromRegistryKey(const wchar_t* registryPath) {
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER, registryPath, 0, KEY_READ | KEY_SET_VALUE, &key);
    if (openResult == ERROR_FILE_NOT_FOUND) {
        return true;
    }
    if (openResult != ERROR_SUCCESS) {
        LogLine("Failed to open registry key: " + Narrow(std::wstring(registryPath)));
        return false;
    }

    std::vector<std::wstring> valuesToDelete;
    DWORD valueCount = 0;
    DWORD maxValueNameLength = 0;
    if (RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &valueCount, &maxValueNameLength, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::wstring valueName(maxValueNameLength + 1, L'\0');
        for (DWORD index = 0; index < valueCount; ++index) {
            DWORD valueNameLength = static_cast<DWORD>(valueName.size());
            DWORD type = 0;
            const LONG enumResult = RegEnumValueW(key, index, valueName.empty() ? nullptr : &valueName[0], &valueNameLength, nullptr, &type, nullptr, nullptr);
            if (enumResult != ERROR_SUCCESS) {
                continue;
            }

            valueName.resize(valueNameLength);
            if (type == REG_DWORD && IsBetterVRRegistryValue(valueName)) {
                valuesToDelete.push_back(valueName);
            }
            valueName.resize(maxValueNameLength + 1);
        }
    }

    bool success = true;
    for (const std::wstring& valueName : valuesToDelete) {
        const LONG deleteResult = RegDeleteValueW(key, valueName.c_str());
        if (deleteResult == ERROR_SUCCESS || deleteResult == ERROR_FILE_NOT_FOUND) {
            LogLine("Removed Vulkan layer registration: " + Narrow(valueName));
        }
        else {
            LogLine("Failed to remove Vulkan layer registration: " + Narrow(valueName));
            success = false;
        }
    }

    RegCloseKey(key);
    return success;
}

static bool RemoveBetterVRLayerRegistryEntries() {
    bool success = true;
    // Clean up entries from the current implicit layers location
    if (!RemoveBetterVREntriesFromRegistryKey(VulkanImplicitLayersRegistryPath)) {
        success = false;
    }
    // Also clean up stale entries from the old explicit layers location
    if (!RemoveBetterVREntriesFromRegistryKey(L"SOFTWARE\\Khronos\\Vulkan\\ExplicitLayers")) {
        success = false;
    }
    return success;
}

static bool RegisterImplicitLayer(const LauncherPaths& paths) {
    if (!RemoveBetterVRLayerRegistryEntries()) {
        return false;
    }

    HKEY key = nullptr;
    DWORD disposition = 0;
    const LONG createResult = RegCreateKeyExW(HKEY_CURRENT_USER, VulkanImplicitLayersRegistryPath, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, &disposition);
    if (createResult != ERROR_SUCCESS) {
        LogLine("Failed to create Vulkan ImplicitLayers registry key");
        return false;
    }

    const DWORD enabled = 0;
    const std::wstring valueName = paths.runtimeLayerJson.wstring();
    const LONG setResult = RegSetValueExW(key, valueName.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    RegCloseKey(key);

    if (setResult != ERROR_SUCCESS) {
        LogLine("Failed to register implicit Vulkan layer: " + Narrow(paths.runtimeLayerJson));
        return false;
    }

    LogLine("Registered implicit Vulkan layer: " + Narrow(paths.runtimeLayerJson));
    return true;
}

static std::vector<fs::path> CollectInstalledGraphicPackPaths(const LauncherPaths& paths) {
    std::vector<fs::path> result;
    result.push_back(paths.launcherDir / "portable" / "graphicPacks" / fs::path(std::string(BundledGraphicPackName)));
    result.push_back(paths.launcherDir / "graphicPacks" / fs::path(std::string(BundledGraphicPackName)));
    if (const std::optional<fs::path> appData = GetEnvironmentPath(L"APPDATA"); appData.has_value()) {
        result.push_back(*appData / "Cemu" / "graphicPacks" / fs::path(std::string(BundledGraphicPackName)));
    }

    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

static bool PatchLayerManifest(const LauncherPaths& paths) {
    std::ifstream input(paths.runtimeLayerJson, std::ios::binary);
    if (!input.is_open()) {
        LogLine("Failed to open layer manifest for patching: " + Narrow(paths.runtimeLayerJson));
        return false;
    }

    std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    input.close();

    // note: hardcodes an absolute path, since I think some older versions didn't support the old relative paths the .json used.
    // this resulted in the vulkan loader blindly ignoring the .DLL, but it'd log in the vulkan loader log that LoadLibrary fails with ERROR_INVALID_PARAMETER (87).

    const std::string absoluteDllPath = Narrow(paths.runtimeLayerDll);
    std::string escapedDllPath;
    escapedDllPath.reserve(absoluteDllPath.size() + 16);
    for (char character : absoluteDllPath) {
        if (character == '\\') {
            escapedDllPath += "\\\\";
        }
        else {
            escapedDllPath += character;
        }
    }

    const std::string oldValue = "\"library_path\": \"${BETTERVR_LAYER_DLL_PATH}\"";
    const std::string newValue = "\"library_path\": \"" + escapedDllPath + "\"";
    const size_t position = contents.find(oldValue);
    if (position == std::string::npos) {
        LogLine("Could not find library_path in layer manifest to patch");
        return false;
    }

    contents.replace(position, oldValue.size(), newValue);

    std::ofstream output(paths.runtimeLayerJson, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
        LogLine("Failed to open layer manifest for writing: " + Narrow(paths.runtimeLayerJson));
        return false;
    }

    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output.good()) {
        LogLine("Failed to write patched layer manifest");
        return false;
    }

    LogLine("Patched layer manifest library_path to: " + absoluteDllPath);
    return true;
}

static bool PrepareInstalledAssets(const LauncherPaths& paths) {
    if (!InstallGraphicPack(paths)) {
        return false;
    }
    if (!ExtractRuntimeFiles(paths)) {
        return false;
    }
    if (!PatchLayerManifest(paths)) {
        return false;
    }
    if (!RegisterImplicitLayer(paths)) {
        return false;
    }

    return true;
}

static bool RemoveInstalledAssets(const LauncherPaths& paths) {
    bool success = true;
    bool removedAnything = false;

    for (const fs::path& packPath : CollectInstalledGraphicPackPaths(paths)) {
        std::error_code ec;
        const uintmax_t removedCount = fs::remove_all(packPath, ec);
        if (ec) {
            LogLine("Failed to remove BetterVR graphic pack: " + Narrow(packPath) + " (" + ec.message() + ")");
            success = false;
            continue;
        }

        if (removedCount > 0) {
            LogLine("Removed BetterVR graphic pack: " + Narrow(packPath));
            removedAnything = true;
        }
    }

    if (!RemoveBetterVRLayerRegistryEntries()) {
        success = false;
    }

    if (!removedAnything) {
        LogLine("No BetterVR installation files were found to remove");
    }

    return success;
}

static void CleanupLegacyLaunchFiles(const LauncherPaths& paths) {
    for (std::wstring_view fileName : LegacyLaunchFiles) {
        const fs::path filePath = paths.launcherDir / fs::path(fileName);
        std::error_code ec;
        const bool removed = fs::remove(filePath, ec);
        if (ec) {
            LogLine("Failed to remove legacy BetterVR launch file: " + Narrow(filePath) + " (" + ec.message() + ")");
            continue;
        }

        if (removed) {
            LogLine("Removed legacy BetterVR launch file: " + Narrow(filePath));
        }
    }
}

static bool ReplaceTextToken(std::string& contents, std::string_view token, const std::string& value) {
    bool replacedAny = false;
    size_t searchPosition = 0;
    while ((searchPosition = contents.find(token, searchPosition)) != std::string::npos) {
        contents.replace(searchPosition, token.size(), value);
        searchPosition += value.size();
        replacedAny = true;
    }

    return replacedAny;
}

static double GetDistanceToNearestInteger(double value) {
    return std::abs(value - std::round(value));
}

static double GetTextureRedefinePenalty(uint32_t width, uint32_t height) {
    double penalty = 0.0;
    for (const BetterVRTextureRedefineRule& rule : BetterVRTextureRedefineRules) {
        const double overwriteWidth = (double(width) / 1280.0) * rule.width;
        const double overwriteHeight = ((double(height) / 720.0) * rule.height) + rule.heightBias;
        penalty += (GetDistanceToNearestInteger(overwriteWidth) + GetDistanceToNearestInteger(overwriteHeight)) * rule.weight;
    }

    return penalty;
}

static uint32_t AlignDown(uint32_t value, uint32_t alignment) {
    return value - (value % alignment);
}

static std::pair<uint32_t, uint32_t> GetNearestRulesCompatibleEyeResolution(double width, double height) {
    if (width <= 0.0 || height <= 0.0) {
        return { 0, 0 };
    }

    struct ResolutionCandidate {
        uint32_t width;
        uint32_t height;
        double distance;
        double penalty;
    };

    const auto evaluateCandidate = [width, height](uint32_t candidateWidth, uint32_t candidateHeight) {
        const double distance = std::hypot(double(candidateWidth) - width, double(candidateHeight) - height);
        return ResolutionCandidate{ candidateWidth, candidateHeight, distance, GetTextureRedefinePenalty(candidateWidth, candidateHeight) };
    };

    const uint32_t targetWidth = std::max(1u, static_cast<uint32_t>(std::llround(width)));
    const uint32_t targetHeight = std::max(1u, static_cast<uint32_t>(std::llround(height)));
    const ResolutionCandidate targetCandidate = evaluateCandidate(targetWidth, targetHeight);
    ResolutionCandidate bestNearbyMatch = targetCandidate;

    const uint32_t searchStartWidth = std::max(BetterVRResolutionSearchAlignment, AlignDown(targetWidth > BetterVRResolutionSearchRadius ? targetWidth - BetterVRResolutionSearchRadius : BetterVRResolutionSearchAlignment, BetterVRResolutionSearchAlignment));
    const uint32_t searchStartHeight = std::max(BetterVRResolutionSearchAlignment, AlignDown(targetHeight > BetterVRResolutionSearchRadius ? targetHeight - BetterVRResolutionSearchRadius : BetterVRResolutionSearchAlignment, BetterVRResolutionSearchAlignment));
    const uint32_t searchEndWidth = targetWidth + BetterVRResolutionSearchRadius;
    const uint32_t searchEndHeight = targetHeight + BetterVRResolutionSearchRadius;

    for (uint32_t candidateWidth = searchStartWidth; candidateWidth <= searchEndWidth; candidateWidth += BetterVRResolutionSearchAlignment) {
        for (uint32_t candidateHeight = searchStartHeight; candidateHeight <= searchEndHeight; candidateHeight += BetterVRResolutionSearchAlignment) {
            const ResolutionCandidate candidate = evaluateCandidate(candidateWidth, candidateHeight);
            if (candidate.distance <= BetterVRPreferredMatchDistance && (candidate.penalty < bestNearbyMatch.penalty || (candidate.penalty == bestNearbyMatch.penalty && candidate.distance < bestNearbyMatch.distance))) {
                bestNearbyMatch = candidate;
            }
        }
    }

    if (bestNearbyMatch.penalty < targetCandidate.penalty) {
        return { bestNearbyMatch.width, bestNearbyMatch.height };
    }

    return { targetCandidate.width, targetCandidate.height };
}

static std::optional<fs::path> GetDownloadedGraphicsOutputPath(const LauncherPaths& paths, std::string_view assetRelativePath) {
    if (!assetRelativePath.starts_with(BundledDownloadedGraphicsPrefix)) {
        return std::nullopt;
    }

    assetRelativePath.remove_prefix(BundledDownloadedGraphicsPrefix.size());
    return paths.downloadedGraphicsDir / fs::path(std::string(assetRelativePath));
}

static bool WriteTemporaryDownloadedGraphicsFile(const fs::path& path, const unsigned char* data, size_t size, TemporaryDownloadedGraphicsFiles& extractedFiles) {
    if (!WriteBinaryFile(path, data, size)) {
        return false;
    }

    extractedFiles.paths.push_back(path);
    return true;
}

static void CleanupDownloadedGraphicsOverrideFiles(const LauncherPaths& paths) {
    for (size_t index = 0; index < EmbeddedAssets::AssetCount; ++index) {
        const EmbeddedAssets::Asset& asset = EmbeddedAssets::Assets[index];
        if (asset.kind != EmbeddedAssets::AssetKind::DownloadedGraphics) {
            continue;
        }

        const std::optional<fs::path> outputPath = GetDownloadedGraphicsOutputPath(paths, asset.relativePath);
        if (!outputPath.has_value()) {
            LogLine("Embedded asset had unexpected downloaded graphics path during cleanup: " + std::string(asset.relativePath));
            continue;
        }

        std::error_code ec;
        fs::remove(*outputPath, ec);
        if (ec && ec.value() != ERROR_FILE_NOT_FOUND) {
            LogLine("Failed to remove stale downloaded graphics override file: " + Narrow(*outputPath) + " (" + ec.message() + ")");
        }
    }
}

static bool MoveDownloadedGraphicsIntoSubfolder(const LauncherPaths& paths) {
    wchar_t transactionDescription[] = L"BetterVR Graphics move";
    HANDLE transactionHandle = CreateTransaction(nullptr, nullptr, 0, 0, 0, 0, transactionDescription);
    if (transactionHandle == INVALID_HANDLE_VALUE) {
        LogLine("Failed to create graphics transaction (error " + std::to_string(GetLastError()) + ")");
        return false;
    }

    auto rollbackAndClose = [&]() {
        if (!RollbackTransaction(transactionHandle)) {
            LogLine("Failed to roll back graphics transaction (error " + std::to_string(GetLastError()) + ")");
        }
        CloseHandle(transactionHandle);
    };

    if (!CreateDirectoryTransactedW(nullptr, paths.downloadedGraphicsSubfolder.c_str(), nullptr, transactionHandle)) {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            LogLine("Failed to create graphics subfolder in transaction: " + Narrow(paths.downloadedGraphicsSubfolder) + " (error " + std::to_string(error) + ")");
            rollbackAndClose();
            return false;
        }
    }

    std::error_code ec;
    fs::directory_iterator iterator(paths.downloadedGraphicsDir, ec);
    if (ec) {
        LogLine("Failed to enumerate downloaded graphics directory: " + ec.message());
        rollbackAndClose();
        return false;
    }

    for (const fs::directory_entry& entry : iterator) {
        if (ec) {
            LogLine("Failed to enumerate downloaded graphics directory: " + ec.message());
            rollbackAndClose();
            return false;
        }

        if (entry.path().filename() == paths.downloadedGraphicsSubfolder.filename()) {
            continue;
        }

        const fs::path destinationPath = paths.downloadedGraphicsSubfolder / entry.path().filename();
        if (!MoveFileTransactedW(entry.path().c_str(), destinationPath.c_str(), nullptr, nullptr, MOVEFILE_WRITE_THROUGH, transactionHandle)) {
            LogLine("Failed to move graphics entry into sentinel subfolder: " + Narrow(entry.path()) + " (error " + std::to_string(GetLastError()) + ")");
            rollbackAndClose();
            return false;
        }
    }

    if (!CommitTransaction(transactionHandle)) {
        LogLine("Failed to commit graphics transaction (error " + std::to_string(GetLastError()) + ")");
        rollbackAndClose();
        return false;
    }

    CloseHandle(transactionHandle);
    LogLine("Moved downloaded BOTW Graphics files into: " + Narrow(paths.downloadedGraphicsSubfolder));
    return true;
}

static bool InstallDownloadedGraphicsOverride(const LauncherPaths& paths, const OpenXRProbeResult& probeResult, TemporaryDownloadedGraphicsFiles& extractedFiles) {
    if (!fs::exists(paths.downloadedGraphicsDir) || !fs::is_directory(paths.downloadedGraphicsDir)) {
        const std::string message =
            "The BotW Community Graphic Packs are not installed yet.\n\n"
            "Please download the Community Graphic Packs by following the setup guide, or in Cemu go to:\n"
            "Options > Graphic Packs > Download Community Graphic Packs";
        LogLine("Downloaded BOTW Graphics directory was not found: " + Narrow(paths.downloadedGraphicsDir));
        MessageBoxA(nullptr, message.c_str(), "BetterVR Launcher", MB_OK | MB_ICONWARNING);
        return true;
    }

    if (!fs::exists(paths.downloadedGraphicsSubfolder)) {
        if (!MoveDownloadedGraphicsIntoSubfolder(paths)) {
            return false;
        }
    }
    else {
        LogLine("Downloaded BOTW Graphics sentinel subfolder already exists: " + Narrow(paths.downloadedGraphicsSubfolder));
    }

    CleanupDownloadedGraphicsOverrideFiles(paths);

    for (size_t index = 0; index < EmbeddedAssets::AssetCount; ++index) {
        const EmbeddedAssets::Asset& asset = EmbeddedAssets::Assets[index];
        if (asset.kind != EmbeddedAssets::AssetKind::DownloadedGraphics) {
            continue;
        }

        const std::optional<EmbeddedAssetView> assetView = LoadEmbeddedAsset(asset);
        if (!assetView.has_value()) {
            extractedFiles.DeleteAll();
            return false;
        }

        const std::optional<fs::path> outputPath = GetDownloadedGraphicsOutputPath(paths, asset.relativePath);
        if (!outputPath.has_value()) {
            LogLine("Embedded asset had unexpected downloaded graphics path: " + std::string(asset.relativePath));
            extractedFiles.DeleteAll();
            return false;
        }

        std::string_view relativePath = asset.relativePath;
        relativePath.remove_prefix(BundledDownloadedGraphicsPrefix.size());

        if (relativePath == "rules.txt") {
            std::string contents(static_cast<const char*>(assetView->data), assetView->size);
            bool replacedAny = false;
            if (probeResult.leftWidth != 0 && probeResult.leftHeight != 0) {
                struct BetterVRPresetPlaceholder {
                    const char* widthToken;
                    const char* heightToken;
                    double multiplier;
                    const char* label;
                };

                static constexpr BetterVRPresetPlaceholder PresetPlaceholders[] = {
                    { "__BETTERVR_05_WIDTH__", "__BETTERVR_05_HEIGHT__", 0.5, "0.5x" },
                    { "__BETTERVR_10_WIDTH__", "__BETTERVR_10_HEIGHT__", 1.0, "1x" },
                    { "__BETTERVR_20_WIDTH__", "__BETTERVR_20_HEIGHT__", 2.0, "2x" },
                    { "__BETTERVR_30_WIDTH__", "__BETTERVR_30_HEIGHT__", 3.0, "3x" },
                    { "__BETTERVR_40_WIDTH__", "__BETTERVR_40_HEIGHT__", 4.0, "4x" },
                };

                for (const BetterVRPresetPlaceholder& preset : PresetPlaceholders) {
                    const std::pair<uint32_t, uint32_t> adjustedResolution = GetNearestRulesCompatibleEyeResolution(double(probeResult.leftWidth) * preset.multiplier, double(probeResult.leftHeight) * preset.multiplier);
                    LogLine("Adjusted BetterVR " + std::string(preset.label) + " resolution: " + std::to_string(adjustedResolution.first) + "x" + std::to_string(adjustedResolution.second));
                    replacedAny |= ReplaceTextToken(contents, preset.widthToken, std::to_string(adjustedResolution.first));
                    replacedAny |= ReplaceTextToken(contents, preset.heightToken, std::to_string(adjustedResolution.second));
                }
            }

            if (!replacedAny) {
                LogLine("Bundled BOTW Graphics rules did not contain BetterVR preset resolution placeholders");
            }

            if (!WriteTemporaryDownloadedGraphicsFile(*outputPath, reinterpret_cast<const unsigned char*>(contents.data()), contents.size(), extractedFiles)) {
                extractedFiles.DeleteAll();
                return false;
            }
        }
        else if (!WriteTemporaryDownloadedGraphicsFile(*outputPath, static_cast<const unsigned char*>(assetView->data), assetView->size, extractedFiles)) {
            extractedFiles.DeleteAll();
            return false;
        }

        LogLine("Installed temporary downloaded graphics override file: " + Narrow(*outputPath));
    }

    return true;
}

static std::string Trim(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }

    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

static std::optional<std::string> ReadDirectBootTitleId(const LauncherPaths& paths) {
    std::ifstream input(paths.settingsIni, std::ios::in);
    if (!input.is_open()) {
        LogLine("BetterVR settings file was not found; skipping direct boot");
        return std::nullopt;
    }

    bool inBetterVRSettings = false;
    bool directBootEnabled = false;
    std::string directBootTitleId;
    std::string line;
    while (std::getline(input, line)) {
        const std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == ';' || trimmed[0] == '#') {
            continue;
        }

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            inBetterVRSettings = trimmed == "[BetterVR][Settings]";
            continue;
        }

        if (!inBetterVRSettings) {
            continue;
        }

        const size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }

        const std::string key = Trim(trimmed.substr(0, separator));
        const std::string value = Trim(trimmed.substr(separator + 1));
        if (_stricmp(key.c_str(), "BootDirectlyIntoGame") == 0) {
            directBootEnabled = _stricmp(value.c_str(), "true") == 0 || value == "1";
        }
        else if (_stricmp(key.c_str(), "BootDirectlyTitleId") == 0) {
            directBootTitleId = value;
        }
    }

    if (!directBootEnabled) {
        return std::nullopt;
    }

    if (directBootTitleId.empty()) {
        LogLine("Direct boot is enabled but the saved BotW title ID is missing or invalid");
        return std::nullopt;
    }

    LogLine("Direct boot enabled for title ID: " + directBootTitleId);
    return directBootTitleId;
}

static void LogVulkanDetails() {
    HMODULE vulkanModule = LoadLibraryW(L"vulkan-1.dll");
    if (vulkanModule == nullptr) {
        LogLine("Vulkan loader DLL was not found");
        return;
    }

    const auto vkCreateInstancePtr = reinterpret_cast<PFN_vkCreateInstance>(GetProcAddress(vulkanModule, "vkCreateInstance"));
    const auto vkEnumerateInstanceLayerPropertiesPtr = reinterpret_cast<PFN_vkEnumerateInstanceLayerProperties>(GetProcAddress(vulkanModule, "vkEnumerateInstanceLayerProperties"));
    const auto vkEnumerateInstanceExtensionPropertiesPtr = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(GetProcAddress(vulkanModule, "vkEnumerateInstanceExtensionProperties"));
    const auto vkGetInstanceProcAddrPtr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkanModule, "vkGetInstanceProcAddr"));
    if (vkCreateInstancePtr == nullptr || vkEnumerateInstanceLayerPropertiesPtr == nullptr || vkEnumerateInstanceExtensionPropertiesPtr == nullptr || vkGetInstanceProcAddrPtr == nullptr) {
        LogLine("Vulkan loader exports were not found");
        FreeLibrary(vulkanModule);
        return;
    }

    uint32_t layerCount = 0;
    if (vkEnumerateInstanceLayerPropertiesPtr(&layerCount, nullptr) == VK_SUCCESS && layerCount > 0) {
        std::vector<VkLayerProperties> layers(layerCount);
        if (vkEnumerateInstanceLayerPropertiesPtr(&layerCount, layers.data()) == VK_SUCCESS) {
            LogLine("Available Vulkan instance layers:");
            for (const VkLayerProperties& layer : layers) {
                LogLine(std::string(" - ") + layer.layerName);
            }
        }
    }

    uint32_t extensionCount = 0;
    if (vkEnumerateInstanceExtensionPropertiesPtr(nullptr, &extensionCount, nullptr) == VK_SUCCESS && extensionCount > 0) {
        std::vector<VkExtensionProperties> extensions(extensionCount);
        if (vkEnumerateInstanceExtensionPropertiesPtr(nullptr, &extensionCount, extensions.data()) == VK_SUCCESS) {
            LogLine("Available Vulkan instance extensions:");
            for (const VkExtensionProperties& extension : extensions) {
                LogLine(std::string(" - ") + extension.extensionName);
            }
        }
    }

    VkApplicationInfo applicationInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
    applicationInfo.pApplicationName = "BetterVR";
    applicationInfo.pEngineName = "Cemu";
    applicationInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    createInfo.pApplicationInfo = &applicationInfo;

    VkInstance instance = VK_NULL_HANDLE;
    if (vkCreateInstancePtr(&createInfo, nullptr, &instance) == VK_SUCCESS) {
        const auto vkEnumeratePhysicalDevicesPtr = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(vkGetInstanceProcAddrPtr(instance, "vkEnumeratePhysicalDevices"));
        const auto vkGetPhysicalDevicePropertiesPtr = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(vkGetInstanceProcAddrPtr(instance, "vkGetPhysicalDeviceProperties"));
        const auto vkDestroyInstancePtr = reinterpret_cast<PFN_vkDestroyInstance>(vkGetInstanceProcAddrPtr(instance, "vkDestroyInstance"));

        if (vkEnumeratePhysicalDevicesPtr != nullptr && vkGetPhysicalDevicePropertiesPtr != nullptr) {
            uint32_t deviceCount = 0;
            if (vkEnumeratePhysicalDevicesPtr(instance, &deviceCount, nullptr) == VK_SUCCESS && deviceCount > 0) {
                std::vector<VkPhysicalDevice> devices(deviceCount);
                if (vkEnumeratePhysicalDevicesPtr(instance, &deviceCount, devices.data()) == VK_SUCCESS) {
                    LogLine("Available Vulkan physical devices:");
                    for (VkPhysicalDevice device : devices) {
                        VkPhysicalDeviceProperties properties{};
                        vkGetPhysicalDevicePropertiesPtr(device, &properties);
                        LogLine(std::string(" - ") + properties.deviceName);
                    }
                }
            }
        }

        if (vkDestroyInstancePtr != nullptr) {
            vkDestroyInstancePtr(instance, nullptr);
        }
    }

    FreeLibrary(vulkanModule);
}

static void EmbedCemuLog(const LauncherPaths& paths) {
    const fs::path& cemuLogPath = paths.cemuLog;
    std::ifstream cemuLog(cemuLogPath, std::ios::in);
    if (!cemuLog.is_open()) {
        LogLine("Cemu log.txt was not found at: " + Narrow(cemuLogPath));
        return;
    }

    LogLine("========================================");
    LogLine("Cemu log.txt contents (" + Narrow(cemuLogPath) + "):");
    LogLine("========================================");

    std::string line;
    while (std::getline(cemuLog, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::lock_guard<std::mutex> lock(g_launcherLogMutex);
        if (g_launcherLog.is_open()) {
            g_launcherLog << "[Cemu] " << line << '\n';
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_launcherLogMutex);
        if (g_launcherLog.is_open()) {
            g_launcherLog.flush();
        }
    }

    LogLine("========================================");
    LogLine("End of Cemu log.txt");
    LogLine("========================================");
}

static void SetLaunchEnvironment() {
    SetEnvironmentVariableW(L"ENABLE_BETTERVR_MOD", L"1");
    SetEnvironmentVariableW(L"DISABLE_VULKAN_OBS_CAPTURE", L"1");
}

static int LaunchCemuAndWait(const LauncherPaths& paths, const std::vector<std::wstring>& forwardedArgs) {
    SetLaunchEnvironment();

    std::wstring commandLine = L"\"" + paths.cemuExe.wstring() + L"\"";
    bool hasExplicitTitleId = false;
    for (const std::wstring& argument : forwardedArgs) {
        if (_wcsicmp(argument.c_str(), L"--title-id") == 0) {
            hasExplicitTitleId = true;
        }
        commandLine += L" ";
        commandLine += argument;
    }

    if (!hasExplicitTitleId) {
        const std::optional<std::string> directBootTitleId = ReadDirectBootTitleId(paths);
        if (directBootTitleId.has_value()) {
            commandLine += L" --title-id ";
            commandLine += std::wstring(directBootTitleId->begin(), directBootTitleId->end());
        }
    }

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInfo{};

    if (!CreateProcessW(paths.cemuExe.c_str(), commandLine.empty() ? nullptr : &commandLine[0], nullptr, nullptr, FALSE, 0, nullptr, paths.launcherDir.c_str(), &startupInfo, &processInfo)) {
        LogLine("CreateProcessW failed");
        return 1;
    }

    LogLine("Closing launcher log to hand off to the VR layer");
    CloseLog();

    WaitForSingleObject(processInfo.hProcess, INFINITE);

    ReopenLog(paths);

    DWORD exitCode = 1;
    GetExitCodeProcess(processInfo.hProcess, &exitCode);
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);

    LogLine("Cemu exited with code " + std::to_string(exitCode));
    return static_cast<int>(exitCode);
}

int wmain(int argc, wchar_t** argv) {
    try {
        const LauncherPaths paths = DetectPaths();
        InitLog(paths);
        TemporaryDownloadedGraphicsFiles downloadedGraphicsOverrideFiles;
        CleanupLegacyLaunchFiles(paths);
        CleanupDownloadedGraphicsOverrideFiles(paths);

        bool uninstallRequested = false;
        bool keepInstalled = false;
        std::vector<std::wstring> forwardedArgs;
        forwardedArgs.reserve(static_cast<size_t>(std::max(argc - 1, 0)));
        for (int index = 1; index < argc; ++index) {
            if (_wcsicmp(argv[index], L"--uninstall") == 0) {
                uninstallRequested = true;
                continue;
            }
            if (_wcsicmp(argv[index], L"--keep-installed") == 0) {
                keepInstalled = true;
                continue;
            }

            forwardedArgs.emplace_back(argv[index]);
        }

        if (uninstallRequested) {
            CleanupLegacyLaunchFiles(paths);
            CleanupDownloadedGraphicsOverrideFiles(paths);
            if (!RemoveInstalledAssets(paths)) {
                ShowErrorMessage("BetterVR uninstall did not complete cleanly. Check BetterVR_log.txt for details.");
                return 1;
            }

            ShowInfoMessage("BetterVR was removed from the graphic pack folders, Vulkan layer registry, and local runtime cache.");
            return 0;
        }

        if (!std::filesystem::exists(paths.cemuExe)) {
            ShowErrorMessage("Cemu.exe was not found next to BetterVR_Launcher.exe.");
            return 1;
        }

        UpdateChecker::CheckForUpdates(paths);

        if (!EnsureOpenXRRuntimeReady()) {
            return 1;
        }

        if (!PrepareInstalledAssets(paths)) {
            ShowErrorMessage("BetterVR could not install its bundled runtime files. Check BetterVR_log.txt for details.");
            return 1;
        }

        const OpenXRProbeResult probeResult = ProbeOpenXR();
        if (!probeResult.errorMessage.empty()) {
            ShowErrorMessage(probeResult.errorMessage + "\n\nCheck BetterVR_log.txt for more details.");
            return 1;
        }

        if (!InstallDownloadedGraphicsOverride(paths, probeResult, downloadedGraphicsOverrideFiles)) {
            ShowErrorMessage("BetterVR could not install its temporary BotW Graphics override. Check BetterVR_log.txt for details.");
            return 1;
        }

        LogVulkanDetails();

        const int exitCode = LaunchCemuAndWait(paths, forwardedArgs);

        EmbedCemuLog(paths);
        downloadedGraphicsOverrideFiles.DeleteAll();

        if (!keepInstalled) {
            LogLine("Cleaning up installed assets after Cemu exit...");
            RemoveInstalledAssets(paths);
        }
        else {
            LogLine("Skipping cleanup because --keep-installed was specified");
        }

        return exitCode;
    }
    catch (const std::exception& exception) {
        ShowErrorMessage(std::string("BetterVR Launcher failed: ") + exception.what());
        return 1;
    }
}
