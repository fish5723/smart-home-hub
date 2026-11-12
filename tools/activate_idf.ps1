# Smart Home Hub - ESP-IDF Environment Activator
# Usage: . .\tools\activate_idf.ps1
# Run from project root: E:\workspace\smart-home-hub

$env:IDF_PATH = "C:\esp_v6.1\.espressif\v6.1\esp-idf"
$env:IDF_TOOLS_PATH = "C:\Espressif\tools"
$env:IDF_PYTHON_ENV_PATH = "$env:IDF_TOOLS_PATH\python\v6.1\venv"
$env:ESP_IDF_VERSION = "6.1"
$env:IDF_CCACHE_ENABLE = "1"
$env:IDF_DEACTIVATE_FILE_PATH = "$env:TEMP\_idf_deactivate.ps1"

$idf_tool_paths = @(
    "$env:IDF_TOOLS_PATH\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin",
    "$env:IDF_TOOLS_PATH\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin",
    "$env:IDF_TOOLS_PATH\esp-clang\esp-21.1.3_20260408\esp-clang\bin",
    "$env:IDF_TOOLS_PATH\cmake\4.0.3\bin",
    "$env:IDF_TOOLS_PATH\openocd-esp32\v0.12.0-esp32-20260703\openocd-esp32\bin",
    "$env:IDF_TOOLS_PATH\ninja\1.12.1",
    "$env:IDF_TOOLS_PATH\idf-exe\1.0.3",
    "$env:IDF_TOOLS_PATH\ccache\4.12.1\ccache-4.12.1-windows-x86_64",
    "$env:IDF_TOOLS_PATH\esp-rom-elfs\20241011",
    "$env:IDF_PYTHON_ENV_PATH\Scripts",
    "$env:IDF_PATH\tools"
)

foreach ($p in $idf_tool_paths) {
    if (Test-Path $p) {
        $env:PATH = "$p;$env:PATH"
    }
}

Write-Host "✅ ESP-IDF $env:ESP_IDF_VERSION environment activated" -ForegroundColor Green
Write-Host "   IDF_PATH: $env:IDF_PATH" -ForegroundColor Cyan
Write-Host "   Target: esp32p4" -ForegroundColor Cyan
