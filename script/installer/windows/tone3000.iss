; Inno Setup script for the TONE3000 Plum Windows installer (Plum Audio's
; unofficial fork build; every id/name/path differs from the official
; TONE3000 installer so both install side by side).
; Compiled in CI with ISCC (preinstalled on GitHub windows-latest runners):
;
;   iscc /DVersion=x.y.z /DForkBuild=N /DArtefactsDir=..\..\..\build\plugin\TONE3000_artefacts\Release script\installer\windows\tone3000.iss
;
; Defines (override with /D on the command line):
;   ForkBuild     fork build number, REQUIRED (git rev-list --count
;                 $(cat FORK_BASE)..HEAD - the number Settings shows)
;   Version       plugin version string, REQUIRED. Pass the contents of the
;                 repo-root VERSION file (single source of truth, no default
;                 here so the installer can never ship a stale version)
;   ArtefactsDir  path to the JUCE Release artefacts dir, absolute or relative
;                 to this script (default ..\..\..\build\plugin\TONE3000_artefacts\Release)
;   OutputDir     where the setup exe is written (default ..\..\..\build)

#ifndef Version
  #pragma error "Version not set - pass /DVersion=x.y.z (from the repo-root VERSION file)"
#endif
#ifndef ForkBuild
  #pragma error "ForkBuild not set - pass /DForkBuild=N (commits since FORK_BASE)"
#endif
#define Product "TONE3000-Plum"
#define DisplayName "TONE3000 Plum"
#define DataFolder "TONE3000 Plum"
#ifndef ArtefactsDir
  #define ArtefactsDir "..\..\..\build\plugin\TONE3000_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\..\build"
#endif

; AAX is only in the installer when the artefact exists (it is built with
; BUILD_AAX=ON and, in CI, PACE-signed beforehand; see sign-aax-windows.ps1).
; Compile-time check so local builds without AAX still compile this script.
#define AaxBundle ArtefactsDir + "\AAX\{#Product}.aaxplugin"
#if DirExists(AaxBundle)
  #define HaveAax
#endif

[Setup]
AppId={{10EF1F85-6B7C-4C54-95A3-E8AB272D04EE}
AppName={#DisplayName}
AppVersion=build {#ForkBuild} (based on TONE3000 v{#Version})
AppPublisher=Plum Audio
AppPublisherURL=https://github.com/Shayshez/tone3000-plugin
AppSupportURL=https://github.com/Shayshez/tone3000-plugin/issues
AppUpdatesURL=https://github.com/Shayshez/tone3000-plugin/releases
VersionInfoVersion={#Version}.{#ForkBuild}
DefaultDirName={autopf64}\{#DisplayName}
DefaultGroupName={#DisplayName}
OutputDir={#OutputDir}
OutputBaseFilename={#Product}-build{#ForkBuild}-windows-x64
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
; InfoBeforeFile (not LicenseFile): shows the MIT text on an Information
; page without forcing an "I accept" step.
InfoBeforeFile=..\..\..\LICENSE
; Branding: dark T3K banner on the welcome/finish pages, mark chip on the
; inner-page header, and the T3K icon on the setup exe itself. The BMP pairs
; are 100% / 200% DPI variants rendered from design/tone3000-wordmark.svg and
; ui/public/t3k-mark.svg (BMP, not PNG: PNG needs Inno 6.5.2+).
WizardStyle=modern
DisableWelcomePage=no
WizardImageFile=wizard-image-100.bmp,wizard-image-200.bmp
WizardSmallImageFile=wizard-small-100.bmp,wizard-small-200.bmp
SetupIconFile=tone3000.ico
UninstallDisplayIcon={app}\{#Product}.exe
DisableDirPage=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
UninstallDisplayName={#DisplayName}

[Types]
Name: "full";   Description: "Full installation"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "standalone"; Description: "Standalone application"; Types: full custom
Name: "vst3";       Description: "VST3 plug-in";           Types: full custom
#ifdef HaveAax
Name: "aax";        Description: "AAX plug-in (Pro Tools)"; Types: full custom
#endif
Name: "clap";       Description: "CLAP plug-in";           Types: full custom
Name: "presets";    Description: "Factory presets";        Types: full custom

[Files]
; Standalone app → Program Files\TONE3000 Plum
Source: "{#ArtefactsDir}\Standalone\{#Product}.exe"; DestDir: "{app}"; Components: standalone; Flags: ignoreversion
; VST3 bundle → Common Files\VST3 (standard VST3 location)
Source: "{#ArtefactsDir}\VST3\{#Product}.vst3\*"; DestDir: "{commoncf64}\VST3\{#Product}.vst3"; Components: vst3; Flags: ignoreversion recursesubdirs createallsubdirs
#ifdef HaveAax
; AAX bundle → Common Files\Avid\Audio\Plug-Ins (standard AAX location)
Source: "{#AaxBundle}\*"; DestDir: "{commoncf64}\Avid\Audio\Plug-Ins\{#Product}.aaxplugin"; Components: aax; Flags: ignoreversion recursesubdirs createallsubdirs
#endif
; CLAP (single file on Windows) → Common Files\CLAP (standard CLAP location)
Source: "{#ArtefactsDir}\CLAP\{#Product}.clap"; DestDir: "{commoncf64}\CLAP"; Components: clap; Flags: ignoreversion
; Factory presets → ProgramData (all-users; PresetManager scans this as system Factory)
Source: "..\..\..\resources\factory-presets\*.t3kpreset"; DestDir: "{commonappdata}\{#DataFolder}\Presets\Factory"; Components: presets; Flags: ignoreversion

[InstallDelete]
; Installs overlay files and never remove presets dropped from (or renamed
; in) the shipped set, so clear the factory folder before copying the new
; one. User presets live elsewhere (%APPDATA%) and are untouched.
Type: files; Name: "{commonappdata}\{#DataFolder}\Presets\Factory\*.t3kpreset"; Components: presets

[Icons]
Name: "{autoprograms}\{#DisplayName}"; Filename: "{app}\{#Product}.exe"; Components: standalone

[Run]
; Install the Microsoft Edge WebView2 Evergreen Runtime when it's missing
; (typical on Windows 10; Windows 11 ships it). The plugin UI on Windows is
; a WebView2 view - without the runtime JUCE silently falls back to the old
; IE control, which can't serve the embedded https://juce.backend/ UI, and
; every format shows a white screen (issue #54). The ~2 MB Evergreen
; Bootstrapper is downloaded when the user clicks Install (see [Code]); it
; then downloads and installs the runtime itself, so this step needs
; internet access. Setup runs elevated (PrivilegesRequired=admin), so
; /silent /install performs a per-machine install. Runs before the
; postinstall launch entry below, so the first launch already has it.
Filename: "{tmp}\MicrosoftEdgeWebView2Setup.exe"; Parameters: "/silent /install"; StatusMsg: "Installing Microsoft Edge WebView2 Runtime..."; Check: ShouldRunWebView2Bootstrapper
Filename: "{app}\{#Product}.exe"; Description: "Launch {#DisplayName}"; Components: standalone; Flags: nowait postinstall skipifsilent

[Code]
// --- WebView2 Evergreen Runtime bootstrap (needs Inno Setup 6.1+) ---------
//
// Detection follows Microsoft's distribution guidance: the runtime is
// installed when EdgeUpdate registers client {F3017226-FE2A-4295-8BDF-
// 00C3A9A7E4C5} with a non-empty pv value that isn't 0.0.0.0.
// https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution
//
// The bootstrapper is fetched at install time from Microsoft's permalink
// instead of being bundled: it only gets downloaded on the machines that
// actually lack the runtime, and bundling wouldn't buy offline installs
// anyway (the bootstrapper itself downloads the full runtime). No SHA256
// pin on the download: Microsoft rotates the bootstrapper binary in place.
var
  WebView2BootstrapperDownloaded: Boolean;
  DownloadPage: TDownloadWizardPage;

function WebView2RuntimeMissing: Boolean;
var
  Version: String;
begin
  // Per-machine install: EdgeUpdate is 32-bit, so on this x64-only installer
  // the key lives in the WOW6432Node view (HKLM32). Per-user install: HKCU
  // Software is not WOW-redirected, plain HKCU is correct.
  Result := True;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
                         'pv', Version) and (Version <> '') and (Version <> '0.0.0.0') then
    Result := False
  else if RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}',
                              'pv', Version) and (Version <> '') and (Version <> '0.0.0.0') then
    Result := False;
end;

function ShouldRunWebView2Bootstrapper: Boolean;
begin
  Result := WebView2BootstrapperDownloaded;
end;

procedure InitializeWizard;
begin
  DownloadPage := CreateDownloadPage('Microsoft Edge WebView2',
                                     'Setup is downloading the WebView2 Runtime installer. TONE3000 Plum needs it to display its interface.',
                                     nil);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  // Runs on the Install click (and is still invoked on silent installs,
  // where Setup simulates the click).
  if (CurPageID = wpReady) and WebView2RuntimeMissing then
  begin
    DownloadPage.Clear;
    DownloadPage.Add('https://go.microsoft.com/fwlink/p/?LinkId=2124703', 'MicrosoftEdgeWebView2Setup.exe', '');
    DownloadPage.Show;
    try
      try
        DownloadPage.Download;
        WebView2BootstrapperDownloaded := True;
      except
        if DownloadPage.AbortedByUser then
          Log('WebView2 bootstrapper download aborted by user.')
        else
          Log('WebView2 bootstrapper download failed: ' + GetExceptionMessage);
        // Don't fail the whole install over this: the plugin files are
        // still worth installing, and the runtime can be added afterwards.
        SuppressibleMsgBox('Setup could not download the Microsoft Edge WebView2 Runtime, which TONE3000 Plum needs to display its interface.'
                           + #13#10#13#10 + 'Please install it later from https://aka.ms/webview2',
                           mbInformation, MB_OK, IDOK);
      end;
    finally
      DownloadPage.Hide;
    end;
  end;
end;
