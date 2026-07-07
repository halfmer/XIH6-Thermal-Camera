@echo off
cd /d "%~dp0"

set "QT_DIR=E:\Qt\Qt_Creato_All\6.10.1\mingw_64"
set "DEPLOY_DIR=%~dp0deploy"

if not exist "build\esp_uart.exe" (
    echo [ERROR] build\esp_uart.exe not found
    pause
    exit /b 1
)

if not exist "%QT_DIR%\bin\windeployqt.exe" (
    echo [ERROR] windeployqt.exe not found
    echo Path: %QT_DIR%\bin\windeployqt.exe
    pause
    exit /b 1
)

echo ============================================
echo   UART Host - Package Script
echo ============================================
echo.

echo [1/4] Cleaning old deploy...
if exist "%DEPLOY_DIR%" (
    rmdir /s /q "%DEPLOY_DIR%" 2>nul
    timeout /t 1 /nobreak >nul
)
mkdir "%DEPLOY_DIR%" 2>nul

echo [2/4] Copying exe...
copy /y "build\esp_uart.exe" "%DEPLOY_DIR%\" >nul
if errorlevel 1 (
    echo [ERROR] Copy failed
    pause
    exit /b 1
)

echo [3/4] Running windeployqt...
"%QT_DIR%\bin\windeployqt.exe" "%DEPLOY_DIR%\esp_uart.exe" --release --no-translations --compiler-runtime >nul 2>&1

echo [4/4] Creating qt.conf and launcher...
(
echo [Paths]
echo Prefix = .
echo Plugins = .
) > "%DEPLOY_DIR%\qt.conf"

(
echo @echo off
echo start "" "%%~dp0esp_uart.exe"
) > "%DEPLOY_DIR%\launch.bat"

echo.
echo ============================================
echo   Package complete!
echo   Output: %DEPLOY_DIR%
echo   Copy this folder to any Windows PC
echo   and run esp_uart.exe directly.
echo ============================================
pause
