@echo off
setlocal

set TOOLCHAIN=cmake/gcc-arm-none-eabi.cmake
set JOBS=8
set PROJECT_NAME=RS485_IO_RF_V2
set ELF=build\release\%PROJECT_NAME%.elf
set HEX=build\release\%PROJECT_NAME%.hex

if "%1"=="build" goto build
if "%1"=="debug" goto debug
if "%1"=="clean" goto clean
if "%1"=="flash" goto flash

echo Usage:
echo   build.bat build   - configure (if needed) + build Release
echo   build.bat debug   - configure (if needed) + build Debug
echo   build.bat clean   - clean build directories
echo   build.bat flash   - configure+build Release, convert to .hex, flash via SWD
goto end

:build
if not exist build\release\build.ninja (
    echo Configuring Release...
    cmake -S . -B build\release -G Ninja ^
        -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% ^
        -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 goto error
)
echo Building Release...
cmake --build build\release -j%JOBS%
goto end

:debug
if not exist build\debug\build.ninja (
    echo Configuring Debug...
    cmake -S . -B build\debug -G Ninja ^
        -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% ^
        -DCMAKE_BUILD_TYPE=Debug
    if errorlevel 1 goto error
)
echo Building Debug...
cmake --build build\debug -j%JOBS%
goto end

:clean
echo Cleaning...
if exist build\release cmake --build build\release --target clean 2>nul
if exist build\debug   cmake --build build\debug   --target clean 2>nul
goto end

:flash
REM One-shot: configure (if needed) + build Release + elf->hex + SWD flash.
REM Always flashes the Release build - Debug is meant for GDB/ST-Link
REM stepping (-O0/-g), not for what actually goes on the device.
if not exist build\release\build.ninja (
    echo Configuring Release...
    cmake -S . -B build\release -G Ninja ^
        -DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN% ^
        -DCMAKE_BUILD_TYPE=Release
    if errorlevel 1 goto error
)
echo Building Release...
cmake --build build\release -j%JOBS%
if errorlevel 1 goto error

if not exist %ELF% (
    echo [!] %ELF% not found after build, aborting flash.
    goto error
)

echo [*] Converting %ELF% to %HEX%...
arm-none-eabi-objcopy -O ihex %ELF% %HEX%
if errorlevel 1 goto error

echo [*] Flashing %HEX%...
STM32_Programmer_CLI -c port=SWD -w %HEX% -v --start
if errorlevel 1 goto error
echo [+] Flash success!
goto end

:error
echo Configure failed!
exit /b 1

:end
endlocal