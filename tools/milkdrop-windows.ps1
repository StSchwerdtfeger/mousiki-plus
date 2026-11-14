<#
.SYNOPSIS
    Builds projectM (the MilkDrop visuals) for Windows and puts it, with the "Cream of the Crop" presets and the MilkDrop
    texture pack, next to mousiki.exe.

.DESCRIPTION
    Called by setup.ps1 (after Mousiki is built) and by packaging/release.py (the installer and the portable zip); it can
    also be run on its own:

        pwsh -File tools\milkdrop-windows.ps1 -Dest build\Release

    What ends up in -Dest (the folder of mousiki.exe):
        projectM-4.dll, projectM-4-playlist.dll   projectM (LGPL-2.1), built here from source with CMake
        milkdrop\presets\                         "Cream of the Crop": 9,795 presets curated by ISOSCELES
        milkdrop\textures\                        the MilkDrop texture pack (the textures many presets use)
        milkdrop\LICENSE-projectM.txt, milkdrop\README.txt, milkdrop\installed.txt

    Mousiki loads the DLLs when the visuals are first asked for; without them the MilkDrop view is simply left out, so a
    failure here costs nothing else.

    Every download is pinned to one commit of its repository and checked file by file: the SHA-256 of every file in the
    archive, listed in order, gives one checksum per archive (the archive's own bytes are not used, so a zip that GitHub
    packs differently still matches, while any changed file does not). A mismatch stops before anything is built or
    installed. projectM is pinned to a commit of its main branch: the 4.1 releases cannot draw offscreen the way Mousiki
    needs (projectm_create_with_opengl_load_proc / projectm_opengl_render_frame_fbo come with 4.2).

    Needs CMake and a C++ compiler (the ones the Mousiki build uses) and about 80 MB of downloads; the build takes a few
    minutes. Running it again does nothing when the installed files match the pins (use -Force to redo them).

.PARAMETER Dest
    The folder of mousiki.exe.

.PARAMETER Cache
    Downloads and the projectM build go here (default: milkdrop-build in the system's temp folder). Kept for the next run.

.PARAMETER Force
    Build and copy again even when the installed files match the pins.

.PARAMETER Generator
    A CMake generator for the projectM build (default: CMake's choice, as for Mousiki itself).

.PARAMETER PrintManifests
    Only print the checksums of the archives in -Cache (to update a pin) and stop.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dest,
    [string]$Cache = '',
    [switch]$Force,
    [string]$Generator = '',
    [switch]$PrintManifests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2
$ProgressPreference = 'SilentlyContinue'   # (Windows PowerShell 5.1 downloads many times slower while it draws the progress bar)

# ---- the pins (keep in step with THIRD_PARTY_LICENSES.txt; to update: change the commit, run with -PrintManifests) ----
$Pins = @(
    @{ Name = 'projectm'
       Url  = 'https://github.com/projectM-visualizer/projectm/archive/eae22de40144ee8ded92f3c04b93a4c2f82adcc0.zip'
       Manifest = '72181d0f93696f3a6e0a2f5b800c649f2ba9c30d6a16f5af0391cb6ddd04ef75' },
    @{ Name = 'projectm-eval'
       Url  = 'https://github.com/projectM-visualizer/projectm-eval/archive/22fb0cfd8f2dfbcd2b68f2443e7f44e19b32c09a.zip'
       Manifest = 'b3b2e8a6f5548c1fe3d61bc1a996807a2f035b77400e94b91271401e3a179385' },
    @{ Name = 'presets'
       Url  = 'https://github.com/projectM-visualizer/presets-cream-of-the-crop/archive/0180df21f5e0bd39b9060cc5de420ed2f1f9e509.zip'
       Manifest = 'ff7768cb1cf936757df67b023f07c36e7d17865f9f681f6ab8d586feb4f3d4f8' },
    @{ Name = 'textures'
       Url  = 'https://github.com/projectM-visualizer/presets-milkdrop-texture-pack/archive/6368812f27bc747b517218fbf89d21d59afce4d9.zip'
       Manifest = '9f9c431fdccfbf047c258b3bb9fd428bdb203b24cb16fcabefc43813078fb062' }
)
$Stamp = (($Pins | ForEach-Object { "$($_.Name) $($_.Manifest)" }) -join "`n") + "`n"

function Write-Step([string]$Message) { Write-Host "==> $Message" -ForegroundColor Cyan }

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

# The files of an archive below its one top folder: relative path -> zip entry (folders and the top folder itself left out).
function Get-ZipFiles($Archive) {
    $files = @{}
    foreach ($e in $Archive.Entries) {
        if ($e.FullName.EndsWith('/')) { continue }
        $slash = $e.FullName.IndexOf('/')
        if ($slash -lt 0) { continue }
        $files[$e.FullName.Substring($slash + 1)] = $e
    }
    return $files
}

# One SHA-256 over "<sha256 of the file> <relative path>`n" for every file, ordered by path (ordinal).
function Get-ZipManifest([string]$ZipPath) {
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $files = Get-ZipFiles $zip
        $names = [string[]]@($files.Keys)
        [Array]::Sort($names, [StringComparer]::Ordinal)
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $sb = New-Object System.Text.StringBuilder
        foreach ($n in $names) {
            $s = $files[$n].Open()
            try { $h = $sha.ComputeHash($s) } finally { $s.Dispose() }
            [void]$sb.Append(([BitConverter]::ToString($h) -replace '-', '').ToLowerInvariant()).Append(' ').Append($n).Append("`n")
        }
        $all = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($sb.ToString()))
        return ([BitConverter]::ToString($all) -replace '-', '').ToLowerInvariant()
    } finally { $zip.Dispose() }
}

# Unpacks the files below the archive's top folder into $Dir (emptied first).
function Expand-ZipFiles([string]$ZipPath, [string]$Dir) {
    if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    $zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $files = Get-ZipFiles $zip
        $made = New-Object 'System.Collections.Generic.HashSet[string]'
        foreach ($n in $files.Keys) {
            $target = [IO.Path]::Combine($Dir, ($n -replace '/', [IO.Path]::DirectorySeparatorChar))
            $parent = [IO.Path]::GetDirectoryName($target)
            if ($made.Add($parent)) { [void][IO.Directory]::CreateDirectory($parent) }
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($files[$n], $target, $true)
        }
    } finally { $zip.Dispose() }
}

# The archive of a pin in the cache, downloaded when it is missing or does not match (once more after a mismatch).
function Get-PinnedArchive($Pin) {
    $zipPath = Join-Path $Cache "$($Pin.Name).zip"
    if (Test-Path -LiteralPath $zipPath) {
        if ((Get-ZipManifest $zipPath) -eq $Pin.Manifest) { return $zipPath }
        Remove-Item -LiteralPath $zipPath -Force
    }
    $got = ''
    for ($try = 1; $try -le 2; $try++) {
        Write-Step "Downloading $($Pin.Name) ($($Pin.Url))"
        $part = "$zipPath.part"
        Invoke-WebRequest -UseBasicParsing -Uri $Pin.Url -OutFile $part
        try { $got = Get-ZipManifest $part } catch { $got = "(not a zip: $($_.Exception.Message))" }
        if ($got -eq $Pin.Manifest) { Move-Item -LiteralPath $part -Destination $zipPath -Force; return $zipPath }
        Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue
        if ($try -eq 1) { Write-Warning "$($Pin.Name): the files do not match the pin - a damaged download is possible, trying once more." }
    }
    throw "$($Pin.Name): the files at $($Pin.Url) do not match the pinned checksum (expected $($Pin.Manifest), got $got). Nothing was built or installed."
}

if ($Cache -eq '') { $Cache = Join-Path ([IO.Path]::GetTempPath()) 'milkdrop-build' }
New-Item -ItemType Directory -Force -Path $Cache | Out-Null
New-Item -ItemType Directory -Force -Path $Dest | Out-Null
$Cache = (Resolve-Path -LiteralPath $Cache).Path
$Dest = (Resolve-Path -LiteralPath $Dest).Path

if ($PrintManifests) {
    foreach ($p in $Pins) {
        $z = Join-Path $Cache "$($p.Name).zip"
        if (Test-Path -LiteralPath $z) { Write-Host "$($p.Name) $(Get-ZipManifest $z)" } else { Write-Host "$($p.Name) (no $z)" }
    }
    return
}

# ---- already there? ----
$mdDir = Join-Path $Dest 'milkdrop'
$stampFile = Join-Path $mdDir 'installed.txt'
# (MSVC builds projectM-4.dll, MinGW libprojectM-4.dll: Mousiki takes either)
$haveLib = { param($n) (Test-Path -LiteralPath (Join-Path $Dest "$n.dll")) -or (Test-Path -LiteralPath (Join-Path $Dest "lib$n.dll")) }
$haveAll = (Test-Path -LiteralPath $stampFile) -and (& $haveLib 'projectM-4') -and (& $haveLib 'projectM-4-playlist')
if ($haveAll -and -not $Force -and ([IO.File]::ReadAllText($stampFile) -eq $Stamp)) {
    Write-Step "MilkDrop (projectM + presets) is already next to mousiki.exe and up to date"
    return
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw "cmake is not on PATH (it is needed to build projectM)" }

# ---- the sources ----
$zips = @{}
foreach ($p in $Pins) { $zips[$p.Name] = Get-PinnedArchive $p }

# ---- build projectM (the unchanged sources of the pin; projectM-eval goes where its git submodule would be) ----
$src = Join-Path $Cache 'src'
$bld = Join-Path $Cache 'build'
$inst = Join-Path $Cache 'install'
$buildStamp = Join-Path $Cache 'build-pins.txt'
if (-not ((Test-Path -LiteralPath $buildStamp) -and ([IO.File]::ReadAllText($buildStamp) -eq $Stamp))) {
    # new pins: a clean build
    foreach ($d in @($src, $bld, $inst)) { if (Test-Path -LiteralPath $d) { Remove-Item -LiteralPath $d -Recurse -Force } }
}
Write-Step "Unpacking projectM"
Expand-ZipFiles $zips['projectm'] $src
Expand-ZipFiles $zips['projectm-eval'] (Join-Path $src 'vendor\projectm-eval')

Write-Step "Configuring projectM"
$cfg = @('-S', $src, '-B', $bld, '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_SHARED_LIBS=ON', '-DENABLE_PLAYLIST=ON',
         '-DENABLE_SYSTEM_GLM=OFF', '-DENABLE_SYSTEM_PROJECTM_EVAL=OFF', '-DENABLE_SDL_UI=OFF', '-DBUILD_TESTING=OFF',
         '-DBUILD_DOCS=OFF', "-DCMAKE_INSTALL_PREFIX=$inst")
if ($Generator -ne '') { $cfg = @('-G', $Generator) + $cfg }
& cmake @cfg
if ($LASTEXITCODE -ne 0) { throw "projectM: cmake configure failed" }
Write-Step "Building projectM (a few minutes)"
$jobs = if ($env:NUMBER_OF_PROCESSORS) { $env:NUMBER_OF_PROCESSORS } else { '2' }
& cmake --build $bld --config Release --parallel $jobs
if ($LASTEXITCODE -ne 0) { throw "projectM: the build failed" }
& cmake --install $bld --config Release
if ($LASTEXITCODE -ne 0) { throw "projectM: cmake --install failed" }
[IO.File]::WriteAllText($buildStamp, $Stamp)

# ---- copy next to mousiki.exe ----
$isWin = [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$libs = @()
foreach ($want in @('projectM-4', 'projectM-4-playlist')) {
    # MSVC: projectM-4.dll in bin\; MinGW: libprojectM-4.dll. (On Linux -- only to try this script there -- the .so files.)
    $pattern = if ($isWin) { "$want.dll", "lib$want.dll" } else { "lib$want.so.*.*.*" }
    $found = Get-ChildItem -LiteralPath $inst -Recurse -File | Where-Object { $n = $_.Name; @($pattern | Where-Object { $n -like $_ }).Count -gt 0 } | Select-Object -First 1
    if (-not $found) { throw "projectM: $want was not found in $inst after the build" }
    $libs += $found
}
Write-Step "Copying projectM and the presets next to mousiki.exe"
# (the names stay as built: the playlist DLL asks for projectM's DLL by its name)
foreach ($l in $libs) { Copy-Item -LiteralPath $l.FullName -Destination (Join-Path $Dest $l.Name) -Force }
if (Test-Path -LiteralPath $mdDir) { Remove-Item -LiteralPath $mdDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $mdDir | Out-Null
Expand-ZipFiles $zips['presets'] (Join-Path $mdDir 'presets')
Expand-ZipFiles $zips['textures'] (Join-Path $mdDir 'textures')
Copy-Item -LiteralPath (Join-Path $src 'COPYING') -Destination (Join-Path $mdDir 'LICENSE-projectM.txt') -Force
Copy-Item -LiteralPath (Join-Path $src 'vendor\projectm-eval\LICENSE.md') -Destination (Join-Path $mdDir 'LICENSE-projectm-eval.md') -Force
Copy-Item -LiteralPath (Join-Path $src 'vendor\hlslparser\LICENSE') -Destination (Join-Path $mdDir 'LICENSE-hlslparser.txt') -Force

$readme = @"
MilkDrop visuals for Mousiki
============================
projectM-4.dll and projectM-4-playlist.dll (next to mousiki.exe) are projectM, the open-source rebuild of the Winamp
visualizer MilkDrop: https://github.com/projectM-visualizer/projectm -- LGPL-2.1 (LICENSE-projectM.txt). They were built
unchanged from this source (the corresponding source code):
  $($Pins[0].Url)
  $($Pins[1].Url)  (projectM-eval, MIT, in vendor\projectm-eval)
Compiled into them (from projectM's vendor folder): projectM-eval (MIT, LICENSE-projectm-eval.md), hlslparser (MIT,
LICENSE-hlslparser.txt), GLM (MIT), glad (a generated OpenGL loader: WTFPL or CC0-1.0, and Apache-2.0 for the Khronos
OpenGL registry it is made from), stb_image (public domain or MIT).
You may replace the two DLLs with your own build of projectM (4.2 or newer); Mousiki loads them at run time.

presets\   "Cream of the Crop" -- 9,795 MilkDrop presets, curated and sorted by ISOSCELES, the default preset pack of
           projectM: https://github.com/projectM-visualizer/presets-cream-of-the-crop (see presets\README.md, presets\LICENSE.md)
           $($Pins[2].Url)
textures\  the MilkDrop texture pack: https://github.com/projectM-visualizer/presets-milkdrop-texture-pack
           $($Pins[3].Url)
The presets were written by many authors over two decades and released freely; see presets\LICENSE.md.

Other presets: set MilkDropPresets= in Mousiki's config.txt (or put them in %LOCALAPPDATA%\projectM\presets and remove this
presets folder).
"@
[IO.File]::WriteAllText((Join-Path $mdDir 'README.txt'), ($readme -replace "`r?`n", "`r`n"))
[IO.File]::WriteAllText($stampFile, $Stamp)
Write-Step "MilkDrop is ready: projectM-4.dll, projectM-4-playlist.dll and milkdrop\ next to mousiki.exe"
