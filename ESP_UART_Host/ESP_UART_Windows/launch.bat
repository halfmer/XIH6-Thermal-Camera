@echo off
cd /d "%~dp0"
if not exist "build\esp_uart.exe" (
    echo [ERROR] build\esp_uart.exe not found
    pause
    exit /b 1
)
start "" "build\esp_uart.exe"
