param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $SimulatorArgs
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$sdl = Join-Path $repo "tools/SDL2-2.30.10"
$build = Join-Path $PSScriptRoot "build"

cmake -S $PSScriptRoot -B $build -G Ninja "-DSDL2_DIR=$sdl"
cmake --build $build --config Debug
& (Join-Path $build "ui_simulator.exe") @SimulatorArgs
exit $LASTEXITCODE
