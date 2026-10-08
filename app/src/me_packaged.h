// Mass Effect for Nintendo Switch: "packaged" mode, the program installed as a full NSP (docs/full-nsp.md).
//
// Two ways to run the same executable:
//
//   NRO mode (default, unchanged): hbloader starts sdmc:/switch/masseffect-nx/masseffect-nx.nro and passes its path
//     as argv[0]. Everything (game_root/, the shader package, masseffect.toml, saves, DLC, caches, logs) lives next
//     to the NRO.
//
//   Packaged mode: the program is the NSO "main" of an installed application whose RomFS carries the read-only data
//     (game_root/, masseffect_shaders.mesp + .idx, masseffect.toml, optional masseffect/0000000000000000/ DLC) and
//     the marker file romfs:/masseffect-nx-package.txt. It is detected in userAppInit (before any static constructor):
//     NSO environment + RomFS mounted + marker present. argv[0] then points at a file in the writable SD folder
//     (data_dir= in the marker, default sdmc:/switch/masseffect-nx-nsp/), so every writable path that derives from
//     the executable folder (saves masseffect/, cache/, logs/, crash files) keeps working unchanged on the SD.
//
// Anything that is neither (NRO without argv, NSO without the marker) behaves exactly as before.

#pragma once

#include <filesystem>
#include <string_view>

namespace rex {
struct PathConfig;
}

namespace me::packaged {

// True when the program runs from an installed NSP with the game data in its RomFS.
bool Active();

// The writable SD folder in packaged mode ("sdmc:/switch/masseffect-nx-nsp"), "" otherwise.
const char* DataDir();

// A read-only data file shipped with the game (masseffect_shaders.mesp, ...): in the RomFS when packaged, otherwise
// next to the executable (the NRO folder), as before.
std::filesystem::path DataFile(std::string_view name);

// A read-only file named by a setting with a relative path (masseffect_native_pipelines_shipped_list, ...). NRO: next
// to the executable, as before. Packaged: the writable SD folder first (a copy the user put there wins, e.g. a list
// that matches a program-only update), then the RomFS. Absolute names are returned unchanged.
std::filesystem::path ShippedFile(const std::filesystem::path& name);

// Packaged mode only (no-op otherwise): game data root = romfs:/game_root, config = <SD folder>/masseffect.toml if
// the user put one there, else romfs:/masseffect.toml, DLC read from romfs:/masseffect when present, VFS index off
// (RomFS listings are in memory). Called from MassEffectApp::OnConfigurePaths, before the config is loaded.
void ConfigurePaths(rex::PathConfig& paths);

// One always-on line on stderr (rex_stderr.log) with the chosen game data root and config and why; called at the
// end of OnConfigurePaths. userAppInit's own steps are written there too (FlushPackagedDiagnostics).
void ReportPaths(const rex::PathConfig& paths);

// One log line describing the mode (after logging is up).
void LogStatus();

}  // namespace me::packaged
