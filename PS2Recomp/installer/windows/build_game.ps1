# Builds ps2EntryRunner.exe on the player's PC from their own game files: the recompiler turns
# extracted\SLES_537.18 into C++ (source\PS2Recomp\ps2xRuntime\src\runner), then MSVC compiles the
# runtime and that code. Nothing of the game is shipped; this is where the game code comes from.
# The compiled VU1 programs come from the same executable: ps2xVuGen reads their code at the places
# source\vu1_locations.toml names.
#
#   build_game.ps1 [-Pace fast|background]
#
#   -Pace fast        all processor threads (as many as the memory allows)
#   -Pace background  half of them, at low priority, so the PC stays usable (asked when not given)
#
# Prints "STEP <text>" and "PROGRESS <0-100>" lines for the installer and the launcher. Exit codes:
# 0 done, 10 no Visual Studio C++ tools, 11 game files missing, 1 failed.
param(
    [ValidateSet("", "fast", "background")][string]$Pace = ""
)
# Exit codes are checked by hand: in Windows PowerShell, "Stop" would turn every line a native tool
# writes to stderr (cmake, ninja) into a fatal error.
$ErrorActionPreference = "Continue"

$app = $PSScriptRoot
$src = Join-Path $app "source"
$root = Join-Path $src "PS2Recomp"
$runtime = Join-Path $root "ps2xRuntime"
$work = Join-Path $app "build"
$cmakeDir = Join-Path $work "cmake"
$elf = Join-Path $app "extracted\SLES_537.18"
$log = Join-Path $app "build_log.txt"

# Started from the Start menu (no -Pace): asks, and keeps the window open at the end.
$interactive = -not $Pace

function Step([string]$text) { Write-Host "STEP $text" }
function Fail([int]$code, [string]$text) { [Console]::Error.WriteLine($text); exit $code }
# Only files whose content changed are replaced, so an identical file keeps its time stamp and
# is not compiled again (the headers are included everywhere, so they matter most).
function Update-File([string]$from, [string]$to) {
    if ((Test-Path $to) -and (Get-Item $to).Length -eq (Get-Item $from).Length -and
        (Get-FileHash $to -Algorithm MD5).Hash -eq (Get-FileHash $from -Algorithm MD5).Hash) { return }
    Copy-Item -Force $from $to
}

New-Item -ItemType Directory -Force $work | Out-Null
Start-Transcript -Path $log -Force | Out-Null
try {
    # The build takes an hour or more, and a PC that sleeps meanwhile stops it. Windows drops this
    # request by itself when the build ends (ES_CONTINUOUS | ES_SYSTEM_REQUIRED; the screen may still turn off).
    try {
        Add-Type -Namespace S2R -Name Power -MemberDefinition '[DllImport("kernel32.dll")] public static extern uint SetThreadExecutionState(uint esFlags);'
        if ([S2R.Power]::SetThreadExecutionState([uint32]"0x80000001") -ne 0) { Write-Host "Keeping the PC awake until the build finishes." }
    } catch { Write-Host "Note: the PC could not be kept awake automatically; keep it from sleeping until the build finishes." }

    if (-not (Test-Path $elf)) { Fail 11 "The game files are not set up yet (extracted\SLES_537.18). Use Change game disc first." }

    # ---- Visual Studio's C++ tools (2022 or newer, with CMake/Ninja) ----
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = $null
    if (Test-Path $vswhere) {
        # Preferably one with clang-cl, which the VU1 sources are built with (else MSVC builds them, slower).
        $cpp = "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "Microsoft.VisualStudio.Component.VC.CMake.Project"
        $vs = & $vswhere -latest -products * -version "[17.0," -requires ($cpp + "Microsoft.VisualStudio.Component.VC.Llvm.Clang") -property installationPath
        if (-not $vs) { $vs = & $vswhere -latest -products * -version "[17.0," -requires $cpp -property installationPath }
    }
    if (-not $vs) { Fail 10 "Visual Studio's C++ tools are not installed. Run S2Recomp Setup again to install them." }

    if (-not $Pace) {
        $answer = Read-Host "Build fast (uses the whole PC) or in the background (PC stays usable)? [F/b]"
        $Pace = if ($answer -match '^[bB]') { "background" } else { "fast" }
    }
    # About 1 GB of memory per compile job: most unity batches peak near 0.4 GB, the few largest near
    # 2.4 GB. Background builds use half the threads at low priority.
    $threads = [Environment]::ProcessorCount
    $freeGb = [math]::Floor((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1MB)
    $jobs = if ($Pace -eq "background") { [math]::Max(1, [math]::Floor($threads / 2)) } else { $threads }
    $jobs = [math]::Max(1, [math]::Min($jobs, $freeGb))
    if ($Pace -eq "background") { (Get-Process -Id $PID).PriorityClass = "BelowNormal" }
    Write-Host "Using $jobs parallel jobs ($Pace, $threads threads, $freeGb GB free memory)"

    # The compiler environment (cl, link, cmake, ninja) for everything below.
    $envLines = cmd /c "`"$vs\Common7\Tools\VsDevCmd.bat`" -arch=amd64 -host_arch=amd64 -no_logo >nul 2>nul && set"
    foreach ($line in $envLines) { if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2]) } }

    # ---- Game code: the recompiler, on the player's own executable ----
    Step "Recompiling the game"
    Write-Host "PROGRESS 0"
    $cfg = Get-Content -Raw (Join-Path $src "sims2_config.toml")
    $gen = Join-Path $work "generated"
    if (Test-Path $gen) { Remove-Item -Recurse -Force $gen }
    $cfg = [regex]::Replace($cfg, '(?m)^input\s*=.*$', "input = `"$($elf.Replace('\', '/'))`"")
    $cfg = [regex]::Replace($cfg, '(?m)^output\s*=.*$', "output = `"$($gen.Replace('\', '/'))/`"")
    [IO.File]::WriteAllText((Join-Path $work "recomp.toml"), $cfg)
    & (Join-Path $app "ps2_recomp.exe") (Join-Path $work "recomp.toml") | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 1 "The recompiler failed (see build_log.txt)." }
    foreach ($h in "ps2_recompiled_functions.h", "ps2_recompiled_stubs.h") { Update-File (Join-Path $gen $h) (Join-Path $runtime "include\$h") }
    $runner = Join-Path $runtime "src\runner"
    New-Item -ItemType Directory -Force $runner | Out-Null
    $generated = @{}
    foreach ($f in Get-ChildItem $gen -File -Filter *.cpp) { $generated[$f.Name] = $true; Update-File $f.FullName (Join-Path $runner $f.Name) }
    Get-ChildItem $runner -File | Where-Object { -not $generated.ContainsKey($_.Name) } | Remove-Item -Force

    # ---- Compile ----
    Step "Configuring"
    # CMake keeps the compiler of the first configure. A build set up with another Visual Studio (one
    # without clang-cl) would go on compiling the VU1 sources with MSVC, hours instead of minutes, so
    # it starts over when the compiler is not this Visual Studio's or clang-cl was not found.
    $cache = Join-Path $cmakeDir "CMakeCache.txt"
    if (Test-Path $cache) {
        $cached = Get-Content $cache
        $compiler = ($cached | Where-Object { $_ -match '^CMAKE_CXX_COMPILER:FILEPATH=' }) -replace '^[^=]+=', ''
        $vsRoot = $vs.Replace('\', '/').TrimEnd('/') + '/'
        $noClang = ($cached -match '^PS2X_CLANG_CL:FILEPATH=.*NOTFOUND') -and (Test-Path "$vs\VC\Tools\Llvm\x64\bin\clang-cl.exe")
        if (-not $compiler.Replace('\', '/').StartsWith($vsRoot, [StringComparison]::OrdinalIgnoreCase) -or $noClang) {
            Write-Host "Configuring again for $vs (was $compiler)"
            Remove-Item -Force $cache
            Remove-Item -Recurse -Force (Join-Path $cmakeDir "CMakeFiles") -ErrorAction SilentlyContinue
        }
    }
    $deps = Join-Path $src "deps"
    # The per-function log tracker and the RPC trace are for development: the tracker writes a line to
    # ps2_log.txt on every game function call. The runtime logs stay (the debug panel's Logs tab).
    cmake -S $root -B $cmakeDir -G Ninja -DCMAKE_BUILD_TYPE=Release `
        -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF `
        -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_IOP_RPC_TRACE=OFF `
        -DFETCHCONTENT_FULLY_DISCONNECTED=ON `
        "-DFETCHCONTENT_SOURCE_DIR_RAYLIB=$deps\raylib" "-DFETCHCONTENT_SOURCE_DIR_IMGUI=$deps\imgui" `
        "-DFETCHCONTENT_SOURCE_DIR_RLIMGUI=$deps\rlimgui" "-DFETCHCONTENT_SOURCE_DIR_SDL2=$deps\sdl2" `
        "-DPS2X_EOS_SDK_DIR=$(Join-Path $src 'EOS\SDK')" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 1 "Configuring the build failed (see build_log.txt)." }

    # ---- VU1 programs: their code from the player's executable -> vu1gen\*.cpp ----
    # Generated next to the build and copied over only where changed (unchanged files are not compiled
    # again). Without them (another version of the game) the VU1 interpreter runs everything.
    Step "Compiling the VU1 programs"
    & cmake --build $cmakeDir --target ps2xVuGen | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail 1 "Building the VU1 program generator failed (see build_log.txt)." }
    $vuOut = Join-Path $work "vu1gen"
    if (Test-Path $vuOut) { Remove-Item -Recurse -Force $vuOut }
    & (Join-Path $cmakeDir "ps2xRuntime\ps2xVuGen.exe") (Join-Path $src "vu1_locations.toml") (Join-Path $app "extracted") $vuOut
    if ($LASTEXITCODE -eq 0) {
        $vuGen = Join-Path $runtime "src\lib\vu\vu1gen"
        $generated = @{}
        foreach ($f in Get-ChildItem $vuOut -File) { $generated[$f.Name] = $true; Update-File $f.FullName (Join-Path $vuGen $f.Name) }
        Get-ChildItem $vuGen -File -Filter vu1p_*.cpp | Where-Object { -not $generated.ContainsKey($_.Name) } | Remove-Item -Force
    }
    else { Write-Host "The VU1 programs could not be generated from this game version; the VU1 interpreter runs them." }

    Step "Compiling the game (this takes a while)"
    $last = -1
    & cmake --build $cmakeDir --target ps2EntryRunner -j $jobs 2>&1 | ForEach-Object {
        $line = "$_"
        if ($line -match '^\[(\d+)/(\d+)\]') {
            $pct = [math]::Floor([int]$matches[1] * 100 / [int]$matches[2])
            if ($pct -ne $last) { Write-Host "PROGRESS $pct"; $last = $pct }
        }
        elseif ($line -match 'error|FAILED') { Write-Host $line }
    }
    if ($LASTEXITCODE -ne 0) { Fail 1 "Compiling failed (see build_log.txt)." }

    Step "Installing the game"
    $built = Join-Path $cmakeDir "ps2xRuntime\ps2EntryRunner.exe"
    Copy-Item -Force $built (Join-Path $app "ps2EntryRunner.exe")
    # Dated now even when nothing changed, so the launcher sees it is newer than this update.
    (Get-Item (Join-Path $app "ps2EntryRunner.exe")).LastWriteTime = Get-Date
    Write-Host "PROGRESS 100"
    Write-Host "The game is built: $(Join-Path $app 'ps2EntryRunner.exe')"
    exit 0
}
finally {
    Stop-Transcript | Out-Null
    if ($interactive) { Read-Host "Press Enter to close" | Out-Null }
}
