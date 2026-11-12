@echo off
:: p4debug.bat - P4 toolchain self-check.
:: Prints which IDF is active and which IDF each build dir belongs to.
::
:: 2026-10-09: switched to ESP-IDF v6.1 together with p4build.bat.
::   This project uses build for standby/wallpaper/weather artifacts.
:: Keep this file ASCII-only with CRLF line endings: cmd.exe mis-parses
:: UTF-8/Chinese bytes in .bat files.
set MSYSTEM=
set ESP_IDF_VERSION=6.1
set IDF_PATH=C:\esp_v6.1\.espressif\v6.1\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.1\venv
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011\
set PATH=%IDF_PATH%\tools;%IDF_PYTHON_ENV_PATH%\Scripts;%IDF_PYTHON_ENV_PATH%\Library\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\idf-exe\1.0.3;%PATH%

python -c "import os; print('MSYS_IN_ENV=', 'MSYSTEM' in os.environ)"
python "%IDF_PATH%\tools\idf.py" --version
echo VER_RC=%ERRORLEVEL%

cd /d E:\workspace\smart-home-hub\firmware\p4-hub
echo.
echo --- build dir ownership (idf_path from project_description.json) ---
if exist build\project_description.json findstr /C:"idf_path" build\project_description.json
echo.
echo NOTE: this project uses the build directory with ESP-IDF 6.1
echo       or just run tools\p4build.bat
