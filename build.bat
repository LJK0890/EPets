@echo off
rem ============================================================
rem  EPets - build ModLoader.dll + 所有模组 + 主程序
rem  产物（项目根目录）: epet.exe  ModLoader.dll  mods\*.dll
rem  中间文件在 build\，构建结束会清掉
rem ============================================================
setlocal
chcp 65001 >nul

for %%I in ("%~dp0.") do set "ROOT=%%~fI"
pushd "%ROOT%" || exit /b 1

rem ---------- 定位 MSVC ----------
set "VCVARS="
for %%P in (
    "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "D:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
) do if not defined VCVARS if exist %%P set "VCVARS=%%~P"

if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" for /f "usebackq delims=" %%V in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find **\vcvarsall.bat`) do if not defined VCVARS set "VCVARS=%%V"

if not defined VCVARS (
    echo [ERROR] vcvarsall.bat not found.
    popd & exit /b 1
)
call "%VCVARS%" x64 >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvarsall failed.
    popd & exit /b 1
)

if not exist build mkdir build
if not exist build\mods mkdir build\mods
del /Q build\*.obj build\*.lib build\*.exp build\*.pdb build\*.ilk 2>nul
del /Q build\mods\*.obj build\mods\*.lib build\mods\*.exp 2>nul

rem ---------- 1. 加载框架 ----------
echo [1/3] ModLoader.dll
cl /nologo /utf-8 /W4 /Imods/incl /Iincl /Fo:build\ /Fd:build\ModLoader.pdb /LD src\ModLoader.c /Fe:ModLoader.dll
if errorlevel 1 goto fail

rem ---------- 2. 模组（有 .c 才编译，方便单独加模组） ----------
echo [2/3] mods
for %%M in (Core Ach Story Minigame Events Activity Care World Debug) do (
    if exist mods\src\%%M.c (
        cl /nologo /utf-8 /W4 /Imods\incl /Fo:build\mods\%%M.obj /Fd:build\mods\%%M.pdb /LD mods\src\%%M.c /Fe:mods\%%M.dll
        if errorlevel 1 goto fail
        echo     mods\%%M.dll
    )
)

rem ---------- 3. 主程序（静态链接 ModLoader.lib） ----------
echo [3/3] epet.exe
cl /nologo /utf-8 /W4 /Imods/incl /Iincl /Fo:build\ /Fd:build\epet.pdb src/main.c src/shell.c /Fe:epet.exe /link /LIBPATH:build ModLoader.lib
if errorlevel 1 goto fail

echo ===== BUILD OK =====
rem 清掉中间文件；产物（epet.exe / ModLoader.dll / mods\*.dll）留在根目录
del /Q build\mods\* 2>nul
del /Q build\* 2>nul
rmdir build\mods /Q
rmdir build /Q
del /Q *.exp *.lib 2>nul
del /Q mods\*.exp mods\*.lib 2>nul
echo 运行: epet.exe（工作目录 = 项目根目录）
popd
endlocal
exit /b 0

:fail
echo ===== BUILD FAILED =====
popd
endlocal
exit /b 1
