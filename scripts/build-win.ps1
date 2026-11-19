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

New-Item -ItemType Directory -Force -Path "$Out\bin" | Out-Null

$common = @(
    "cc", "-target", $Target, "-O2", "-g0",
    "-Wall", "-Wextra", "-Wno-unused-parameter",
    "-I", "src", "-I", $SdlInc, "-I", $TtfInc
)

Write-Host "=== overlayd"
& $Zig @common src/util.c src/overlayd.c -o "$Out/bin/overlayd"
if ($LASTEXITCODE -ne 0) { throw "overlayd build failed" }

Write-Host "=== overlay_ui"
& $Zig @common src/util.c src/overlay_ui.c `
    -L $SdlLib -lSDL2 -lSDL2_ttf -lm -o "$Out/bin/overlay_ui"
if ($LASTEXITCODE -ne 0) { throw "overlay_ui build failed" }

Write-Host "=== vpad (test tool)"
& $Zig @common tools/vpad.c -o "$Out/bin/vpad"
if ($LASTEXITCODE -ne 0) { throw "vpad build failed" }

Write-Host "=== release package"
$pkg = "$Out\OverLyder"
New-Item -ItemType Directory -Force -Path "$pkg\bin", "$pkg\resources", "$pkg\defaults" | Out-Null
Copy-Item "app\config.json", "app\icon.png", "app\launch.sh", "app\overlayctl", `
          "app\overLyder.start", "app\updater.sh" -Destination $pkg -Force
Copy-Item "app\defaults\overlay.cfg" -Destination "$pkg\defaults" -Force
Copy-Item "app\resources\*" -Destination "$pkg\resources" -Force
Copy-Item "$Out\bin\overlayd", "$Out\bin\overlay_ui" -Destination "$pkg\bin" -Force

Write-Host "=== done"
Get-ChildItem "$Out\bin" | Select-Object Name, Length
Get-ChildItem $pkg -Recurse -File | Select-Object FullName, Length
