#pragma once

// Folder name for everything this build stores on disk: settings, user and
// factory presets, the log, the local-model stash, WebView2 data. Deliberately
// not upstream's "TONE3000": this fork's saved state has diverged (scenes,
// Dual Mono, ...), and sharing the folder with an official install would let
// each overwrite or misread the other's files. Must match the installers'
// factory-preset destinations (script/create-pkg.sh, tone3000.iss).
inline constexpr const char* kAppFolderName = "TONE3000 Plum";
