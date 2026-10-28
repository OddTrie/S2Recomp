# Builds the Windows installer: S2Recomp-<version>-windows-x64-setup.exe in installer\build\dist.
#
# Uses the S2RecompLauncher.exe that BUILD_AND_RUN.bat already built (build_win\ps2xRuntime\Release),
# compiles s2r_setup.exe and the disc setup tool, stages them with the DLLs ps2EntryRunner.exe loads and
# the MSVC runtime, and runs Inno Setup 6 (winget install JRSoftware.InnoSetup). The installer holds no
# game data and no ps2EntryRunner.exe (it contains the recompiled game code); both are checked for.
#
#   powershell -ExecutionPolicy Bypass -File package_windows.ps1 [-Version 0.1.0]
param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = "$PSScriptRoot\..\..\build_win"
)
$ErrorActionPreference = "Stop"

$installer = Resolve-Path "$PSScriptRoot\.."
$runtimeSrc = Resolve-Path "$PSScriptRoot\..\..\ps2xRuntime"
$release = Join-Path (Resolve-Path $BuildDir) "ps2xRuntime\Release"
$work = Join-Path $installer "build"
$stage = Join-Path $work "stage\windows"
$dist = Join-Path $work "dist"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ workload was not found." }
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw "Inno Setup 6 was not found (winget install JRSoftware.InnoSetup)." }
if (-not (Test-Path "$release\S2RecompLauncher.exe")) { throw "$release\S2RecompLauncher.exe not found. Run BUILD_AND_RUN.bat first." }

function Invoke-Cl([string]$arguments) {
    cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -no_logo >nul 2>nul && cl /nologo /std:c++17 /O2 /EHsc /W4 /utf-8 /MT $arguments"
    if ($LASTEXITCODE -ne 0) { throw "cl failed: $arguments" }
}

Write-Host "== Tools"
New-Item -ItemType Directory -Force "$work\obj" | Out-Null
Push-Location "$work\obj"
try {
    Invoke-Cl "`"$installer\s2r_setup.cpp`" /Fe:`"$work\s2r_setup.exe`""
    # The disc tool carries the program icon too (the Change game disc shortcut shows it).
    cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -no_logo >nul 2>nul && rc /nologo /fo `"$work\obj\s2recomp.res`" `"$runtimeSrc\assets\s2recomp.rc`""
    if ($LASTEXITCODE -ne 0) { throw "rc failed" }
    Invoke-Cl "/DUNICODE /D_UNICODE `"$installer\windows\s2r_disc.cpp`" `"$work\obj\s2recomp.res`" /Fe:`"$work\S2Recomp Disc Setup.exe`" /link /SUBSYSTEM:WINDOWS user32.lib comdlg32.lib"
} finally { Pop-Location }

Write-Host "== Staging $stage"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$release\S2RecompLauncher.exe", "$work\S2Recomp Disc Setup.exe", "$work\s2r_setup.exe" $stage
# FFmpeg, SDL2 and the libraries they load, for a ps2EntryRunner.exe placed next to the launcher.
Copy-Item "$release\*.dll" $stage
# Online co-op: the Epic Online Services product's client credentials (the game and the Lobbies tab read them).
if (Test-Path "$release\eos.ini") { Copy-Item "$release\eos.ini" $stage } else { Write-Warning "No eos.ini in ${release}: online co-op will not work." }
# The game links the MSVC runtime dynamically; ship it next to the exe so no VC++ redistributable is needed.
$crt = Get-ChildItem "$vs\VC\Redist\MSVC\*\x64\Microsoft.VC*.CRT" -Directory | Sort-Object FullName | Select-Object -Last 1
if (-not $crt) { throw "The MSVC x64 runtime (VC\Redist\MSVC\...\x64\Microsoft.VC*.CRT) was not found." }
Copy-Item "$($crt.FullName)\*.dll" $stage
Copy-Item -Recurse "$runtimeSrc\mods" "$stage\mods"
Copy-Item -Recurse "$runtimeSrc\texpacks" "$stage\texpacks"
Copy-Item "$runtimeSrc\..\LICENSE" "$stage\LICENSE.txt"
# The launcher compares this with the latest GitHub release to offer updates.
Set-Content "$stage\version.txt" $Version -Encoding ascii

# What the player's PC builds the game from (build_game.ps1): the recompiler, the runtime's source
# without any generated game code, the libraries it uses (so no git is needed), and the game's config.
$buildRoot = Resolve-Path $BuildDir
Copy-Item "$buildRoot\ps2xRecomp\Release\ps2_recomp.exe", "$installer\windows\build_game.ps1" $stage
$bundle = "$stage\source"
$repo = Resolve-Path "$runtimeSrc\.."
robocopy $repo "$bundle\PS2Recomp" CMakeLists.txt LICENSE /NJH /NJS /NP /NFL /NDL | Out-Null
foreach ($dir in "ps2xRuntime", "ps2xIOP") {
    robocopy "$repo\$dir" "$bundle\PS2Recomp\$dir" /E /NJH /NJS /NP /NFL /NDL `
        /XD "$repo\ps2xRuntime\src\runner" "$repo\ps2xRuntime\src\lib\vu\vu1gen" "$repo\ps2xRuntime\src\lib\vu\pgo" "$repo\ps2xRuntime\vita" .git `
        /XF ps2_recompiled_functions.h ps2_recompiled_stubs.h | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $dir failed" }
}
New-Item -ItemType Directory -Force "$bundle\PS2Recomp\ps2xRuntime\src\runner", "$bundle\PS2Recomp\ps2xRuntime\src\lib\vu\vu1gen" | Out-Null
# No compiled VU1 programs: build_game.ps1 generates them from the player's executable (ps2xVuGen,
# vu1_locations.toml); until then, the registry --vu-gen writes for an empty folder. The clang PGO
# profile of those programs ships (counts per function, no code).
Copy-Item "$installer\vu1_registry_empty.cpp" "$bundle\PS2Recomp\ps2xRuntime\src\lib\vu\vu1gen\vu1_registry.cpp"
Copy-Item "$runtimeSrc\src\lib\vu\vu1gen\vu1.profdata" "$bundle\PS2Recomp\ps2xRuntime\src\lib\vu\vu1gen\vu1.profdata"
foreach ($dep in "raylib", "imgui", "rlimgui", "sdl2") {
    robocopy "$buildRoot\_deps\$dep-src" "$bundle\deps\$dep" /E /NJH /NJS /NP /NFL /NDL /XD .git examples projects | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "Copying $dep failed" }
}
Copy-Item "$repo\..\sims2_config.toml", "$repo\..\vu1_locations.toml" $bundle
# The EOS C SDK's headers, import library and DLL, so the game built on the player's PC has online co-op.
$eosSdk = "$repo\..\..\EOS\SDK"
if (-not (Test-Path "$eosSdk\Include\eos_sdk.h")) { throw "The EOS SDK was not found at $eosSdk." }
robocopy "$eosSdk\Include" "$bundle\EOS\SDK\Include" /E /NJH /NJS /NP /NFL /NDL | Out-Null
if ($LASTEXITCODE -ge 8) { throw "Copying the EOS SDK headers failed" }
New-Item -ItemType Directory -Force "$bundle\EOS\SDK\Lib", "$bundle\EOS\SDK\Bin" | Out-Null
Copy-Item "$eosSdk\Lib\EOSSDK-Win64-Shipping.lib" "$bundle\EOS\SDK\Lib"
Copy-Item "$eosSdk\Bin\EOSSDK-Win64-Shipping.dll" "$bundle\EOS\SDK\Bin"

# No game data and no game code: not the runner, not the disc's files, not recompiled or recorded code.
# The one profile allowed is the VU1 programs' (vu1gen\vu1.profdata).
$vuProfile = "$bundle\PS2Recomp\ps2xRuntime\src\lib\vu\vu1gen\vu1.profdata"
$forbidden = Get-ChildItem -Recurse $stage | Where-Object {
    $_.Name -match '^(ps2EntryRunner.*|SLES_.*|SYSTEM\.CNF|IOPRP.*\.IMG|.*\.NGH|.*\.iso|sub_[0-9A-F]{8}_.*\.cpp|entry_.*_0x.*\.cpp|vu1p_.*\.cpp|.*\.vur|.*\.profdata|ps2_recompiled_.*\.h)$' -and
        $_.FullName -ne $vuProfile
}
if ($forbidden) { $forbidden.FullName | Select-Object -First 20; throw "The installer must not contain the runner, game files or game code (above)." }

Write-Host "== Inno Setup"
New-Item -ItemType Directory -Force $dist | Out-Null
# Antivirus scanning the half-written setup.exe can make ISCC's resource update fail (EndUpdateResource,
# error 110); it passes on a retry.
for ($try = 1; $try -le 3; $try++) {
    & $iscc /Q "/DAppVersion=$Version" "/DStage=$stage" "/DOutputDir=$dist" "$installer\windows\S2Recomp.iss"
    if ($LASTEXITCODE -eq 0) { break }
    if ($try -lt 3) { Write-Warning "ISCC failed (attempt $try of 3), retrying..."; Start-Sleep -Seconds 3 }
}
if ($LASTEXITCODE -ne 0) { throw "ISCC failed. If it says EndUpdateResource, exclude $dist from your antivirus." }
Get-ChildItem "$dist\S2Recomp-$Version-windows-x64-setup.exe" | ForEach-Object { "Built {0} ({1:N0} MB)" -f $_.FullName, ($_.Length / 1MB) }
