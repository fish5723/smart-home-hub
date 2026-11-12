@echo off
:: p4build.bat - P4 firmware one-shot build (called from Git Bash via cmd)
::
:: Fixes applied (2026-10):
::  1. set MSYSTEM=          - idf.py skips main() if MSYSTEM is present
::  2. python explicit       - .py is associated with PyCharm on this machine
::  3. ESP_IDF_VERSION       - esp_wifi_remote Kconfig orsources
::     idf_v$ESP_IDF_VERSION/Kconfig.slave_select.in; without this env the
::     slave choice has no members -> "Unknown Slave Target" compile error
::  4. IDF v6.1 (2026-10-09) - project moved to ESP-IDF v6.1. Build dir is
::     build is the only supported project build directory.
::     The project now uses ESP-IDF v6.1 exclusively.
:: Keep this file ASCII-only with CRLF line endings: cmd.exe mis-parses
:: UTF-8/Chinese bytes in .bat files.
set MSYSTEM=
set ESP_IDF_VERSION=6.1
set IDF_PATH=C:\esp_v6.1\.espressif\v6.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011\
set PATH=%IDF_PATH%\tools;%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PYTHON_ENV_PATH%\Library\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\idf-exe\1.0.3;%PATH%
cd /d E:\workspace\smart-home-hub\firmware\p4-hub
:: if sdkconfig is missing (clean checkout) the target memory is lost and
:: idf.py falls back to esp32 -> xtensa toolchain error
if not exist sdkconfig python "%IDF_PATH%\tools\idf.py" -B build set-target esp32p4
python "%IDF_PATH%\tools\idf.py" -B build build
