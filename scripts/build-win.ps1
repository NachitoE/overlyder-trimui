param(
    [string]$Zig = (Join-Path $env:TEMP "zig\zig-x86_64-windows-0.14.1\zig.exe"),
    [string]$SdlInc = (Join-Path $env:TEMP "sdl\SDL\include"),
    [string]$TtfInc = (Join-Path $env:TEMP "sdl\SDL_ttf"),
    [string]$SdlLib = (Join-Path $env:TEMP "libs"),
    [string]$Target = "aarch64-linux-gnu.2.33",
    [string]$Out = "dist"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# The include folders are extracted from the SDL zip releases. Their exact
# name changes with the version (SDL2-2.30.0, SDL2_ttf-2.22.0, ...), so accept
# the historical names and fall back to whatever is under %TEMP%\sdl.
function Resolve-Include([string]$hint, [string]$pattern) {
    if (Test-Path $hint) { return $hint }
    $dir = Get-ChildItem -Path (Join-Path $env:TEMP "sdl") -Directory -Filter $pattern -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($dir) { return (Join-Path $dir.FullName "include") }
    return $hint
}
$SdlInc = Resolve-Include $SdlInc "SDL2-*"
$TtfInc = Resolve-Include $TtfInc "SDL2_ttf-*"
if (-not (Test-Path (Join-Path $SdlInc "SDL.h"))) {
    throw "SDL2 headers not found at '$SdlInc'. Unzip SDL2 into %TEMP%\\sdl or pass -SdlInc <path>."
}
if (-not (Test-Path (Join-Path $TtfInc "SDL_ttf.h"))) {
    throw "SDL_ttf headers not found at '$TtfInc'. Unzip SDL_ttf into %TEMP%\\sdl or pass -TtfInc <path>."
}
if (-not (Test-Path $SdlLib)) {
    throw "SDL2 aarch64 libraries not found at '$SdlLib'. Copy libSDL2-2.0.so.0 and libSDL2_ttf-2.0.so.0 from the device's /usr/trimui/lib/ into a folder, name them libSDL2.so / libSDL2_ttf.so, and pass -SdlLib <folder>."
}

New-Item -ItemType Directory -Force -Path "$Out\bin" | Out-Null

$common = @(
    "cc", "-target", $Target, "-O2", "-g0",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", "src", "-I", $SdlInc, "-I", $TtfInc
)

Write-Host "=== overlayd"
& $Zig @common src/util.c src/overlay_layer.c src/textinject.c src/overlayd.c -o "$Out/bin/overlayd"
if ($LASTEXITCODE -ne 0) { throw "overlayd build failed" }

Write-Host "=== overlay_ui"
& $Zig @common src/util.c src/overlay_layer.c src/textinject.c src/overlay_ui.c `
    -L $SdlLib -lSDL2 -lSDL2_ttf -lm -o "$Out/bin/overlay_ui"
if ($LASTEXITCODE -ne 0) { throw "overlay_ui build failed" }

Write-Host "=== fpshud"
& $Zig @common src/util.c src/overlay_layer.c src/fpshud.c `
    -L $SdlLib -lSDL2 -lSDL2_ttf -lm -o "$Out/bin/fpshud"
if ($LASTEXITCODE -ne 0) { throw "fpshud build failed" }

Write-Host "=== frame limiter shim (LD_PRELOAD)"
New-Item -ItemType Directory -Force -Path "$Out\lib" | Out-Null
& $Zig @common -shared -fPIC src/pace_shim.c -ldl -o "$Out/lib/liboverlyder_pace.so"
if ($LASTEXITCODE -ne 0) { throw "pace shim build failed" }

Write-Host "=== vpad (test tool)"
& $Zig @common tools/vpad.c -o "$Out/bin/vpad"
if ($LASTEXITCODE -ne 0) { throw "vpad build failed" }

Write-Host "=== release package"
$pkg = "$Out\OverLyder"
New-Item -ItemType Directory -Force -Path "$pkg\bin", "$pkg\resources", "$pkg\defaults", "$pkg\lang", "$pkg\lib" | Out-Null
Copy-Item "app\config.json", "app\manifest.json", "app\icon.png", "app\launch.sh", "app\overlayctl", `
          "app\overLyder.start", "app\updater.sh" -Destination $pkg -Force
Copy-Item "app\defaults\overlay.cfg" -Destination "$pkg\defaults" -Force
Copy-Item "app\resources\*" -Destination "$pkg\resources" -Force
Copy-Item "$Out\bin\overlayd", "$Out\bin\overlay_ui", "$Out\bin\fpshud" -Destination "$pkg\bin" -Force
Copy-Item "$Out\lib\liboverlyder_pace.so" -Destination "$pkg\lib" -Force
Copy-Item "app\lang\*.json" -Destination "$pkg\lang" -Force

Write-Host "=== done"
Get-ChildItem "$Out\bin" | Select-Object Name, Length
Get-ChildItem $pkg -Recurse -File | Select-Object FullName, Length
