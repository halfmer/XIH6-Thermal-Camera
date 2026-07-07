@echo off
echo === 添加 Qt 开发工具到系统 PATH ===
echo.
echo   CMake 4.2.0       : E:\Qt\Cmake\bin
echo   MinGW 13.1.0      : E:\Qt\Qt_Creato_All\Tools\mingw1310_64\bin
echo   Ninja             : E:\Qt\Qt_Creato_All\Tools\Ninja
echo   Qt 6.10.1 (mingw) : E:\Qt\Qt_Creato_All\6.10.1\mingw_64\bin
echo   Qt 6.5.7          : E:\Qt\qt_install\bin
echo.

setlocal EnableDelayedExpansion
set "NEW_PATH=%PATH%"

for %%P in (
    "E:\Qt\Cmake\bin"
    "E:\Qt\Qt_Creato_All\Tools\mingw1310_64\bin"
    "E:\Qt\Qt_Creato_All\Tools\Ninja"
    "E:\Qt\Qt_Creato_All\6.10.1\mingw_64\bin"
    "E:\Qt\qt_install\bin"
) do (
    echo !PATH! | findstr /i /c:%%~P >nul
    if !errorlevel! equ 0 (
        echo   [已存在] %%~P
    ) else (
        echo   [添加] %%~P
        set "NEW_PATH=!NEW_PATH!;%%~P"
    )
)

echo.
setx /M PATH "!NEW_PATH!"
if !errorlevel! equ 0 (
    echo === 系统 PATH 更新成功! ===
) else (
    echo === 更新失败，请右键以管理员身份运行此脚本! ===
)
echo.
pause
