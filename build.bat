@echo off
setlocal EnableExtensions DisableDelayedExpansion
rem Standalone release build: no PowerShell or Python dependency.
pushd "%~dp0" || exit /b 1
set "HV_ROOT=%CD%"
set "HV_STAGE=%CD%\x64\.build"
set "HV_RELEASE=%CD%\x64\Release"
set "HV_C_DEBUG="
set "HV_ASM_DEBUG="
set "HV_LINK_DEBUG=/debug:none"
if /i "%~1"=="diagnostic" (
    set "HV_STAGE=%CD%\x64\.build-diagnostic"
    set "HV_RELEASE=%CD%\x64\Diagnostic"
    set "HV_C_DEBUG=/Z7"
    set "HV_ASM_DEBUG=/Zi"
    set "HV_LINK_DEBUG=/debug:full"
)
if not "%~2"=="" goto :usage
if not "%~1"=="" if /i not "%~1"=="release" if /i not "%~1"=="diagnostic" if /i not "%~1"=="check" goto :usage
call :findTools
if errorlevel 1 goto :missingTools
call "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64 %WDK_VER%
if errorlevel 1 goto :error
where cl.exe >nul 2>&1 || goto :missingTools
where ml64.exe >nul 2>&1 || goto :missingTools
where link.exe >nul 2>&1 || goto :missingTools
where dumpbin.exe >nul 2>&1 || goto :missingTools
echo Visual Studio: "%VS_PATH%"
echo Windows kit: "%WDK_ROOT%" version %WDK_VER%
if /i "%~1"=="check" (
    echo Toolchain discovery passed. No binaries were built.
    popd
    exit /b 0
)
set "CL="
set "_CL_="
set "LINK="
set "_LINK_="
for %%D in ("%HV_ROOT%" "%HV_ROOT%\x64" "%HV_STAGE%" "%HV_RELEASE%" "%HV_STAGE%\Hypervisor" "%HV_STAGE%\Console" "%HV_STAGE%\Temp") do (
    call :checkDirectory "%%~D"
    if errorlevel 1 goto :error
)
for %%D in ("%HV_STAGE%\Hypervisor" "%HV_STAGE%\Console" "%HV_STAGE%\Temp" "%HV_RELEASE%") do (
    if not exist "%%~D\" mkdir "%%~D"
    if errorlevel 1 goto :error
)
set "TEMP=%HV_STAGE%\Temp"
set "TMP=%TEMP%"
set "HV_MODULES=driver percpu vmcs vmxon vmexit ept hook persist native syscall filter vpid tsc monitor kernel_telemetry processor"
echo [1/5] Compiling hypervisor
for %%M in (%HV_MODULES%) do (
    cl.exe /nologo /c /TC /std:c11 /kernel /O2 /Oi /GS /Zl /Zp8 /W4 /WX %HV_C_DEBUG% ^
        /D_AMD64_ /D_WIN64 /D_WIN32_WINNT=0x0A00 /DNTDDI_VERSION=0x0A000008 ^
        /Ihypervisor\include /Ishared /I"%WDK_ROOT%\Include\%WDK_VER%\km" ^
        /I"%WDK_ROOT%\Include\%WDK_VER%\shared" /I"%WDK_ROOT%\Include\%WDK_VER%\ucrt" ^
        /Fo"%HV_STAGE%\Hypervisor\%%M.obj" "hypervisor\src\%%M.c"
    if errorlevel 1 goto :error
)
ml64.exe /nologo /c /W3 /WX %HV_ASM_DEBUG% /Fo"%HV_STAGE%\Hypervisor\vmx.obj" hypervisor\src\vmx.asm
if errorlevel 1 goto :error
rem Explicit response list excludes stale/unrelated object files.
>"%HV_STAGE%\driver.rsp" (
    for %%M in (%HV_MODULES% vmx) do echo "%HV_STAGE%\Hypervisor\%%M.obj"
)
echo [2/5] Linking driver
link.exe /nologo /WX /driver /subsystem:native,10.00 /machine:x64 /entry:GsDriverEntry ^
    /nodefaultlib /release /integritycheck /nxcompat /dynamicbase /opt:ref /opt:icf ^
    %HV_LINK_DEBUG% /pdb:"%HV_STAGE%\velum.pdb" /nocoffgrpinfo /incremental:no /out:"%HV_STAGE%\velum.sys" ^
    /libpath:"%WDK_ROOT%\Lib\%WDK_VER%\km\x64" @"%HV_STAGE%\driver.rsp" ^
    ntoskrnl.lib hal.lib BufferOverflowK.lib Wdmsec.lib Aux_Klib.lib
if errorlevel 1 goto :error
echo [3/5] Building user-mode console
cl.exe /nologo /TC /std:c11 /W4 /WX /O2 /GS /MT /Ishared %HV_C_DEBUG% ^
    /Fo"%HV_STAGE%\Console\\" /Fe"%HV_STAGE%\velum-console.exe" ^
    user-mode\console\main.c user-mode\console\cpuid.c user-mode\console\service.c user-mode\console\monitor.c user-mode\console\input.c user-mode\console\processor.c ^
    /link advapi32.lib %HV_LINK_DEBUG% /pdb:"%HV_STAGE%\velum-console.pdb" /nocoffgrpinfo /incremental:no /opt:ref /opt:icf ^
    /nxcompat /dynamicbase /manifest:embed "/manifestuac:level='requireAdministrator' uiAccess='false'"
if errorlevel 1 goto :error
echo [4/5] Verifying PE headers
for %%F in (velum.sys velum-console.exe) do (
    dumpbin.exe /nologo /headers "%HV_STAGE%\%%F" > "%HV_STAGE%\%%F.headers.txt"
    if errorlevel 1 goto :error
    call :verifyPE "%HV_STAGE%\%%F.headers.txt"
    if errorlevel 1 goto :error
)
echo [5/5] Publishing local build output
copy /y "%HV_STAGE%\velum.sys" "%HV_RELEASE%\velum.sys" >nul
if errorlevel 1 goto :error
copy /y "%HV_STAGE%\velum-console.exe" "%HV_RELEASE%\velum-console.exe" >nul
if errorlevel 1 goto :error
if /i "%~1"=="diagnostic" (
    copy /y "%HV_STAGE%\velum.pdb" "%HV_RELEASE%\velum.pdb" >nul
    if errorlevel 1 goto :error
    copy /y "%HV_STAGE%\velum-console.pdb" "%HV_RELEASE%\velum-console.pdb" >nul
    if errorlevel 1 goto :error
    echo Diagnostic symbols and intermediates retained. Optimization matches release.
    goto :published
)
rem Only known artifacts are removed. Empty-directory removal is non-recursive.
for %%M in (%HV_MODULES% vmx) do del /q "%HV_STAGE%\Hypervisor\%%M.obj" >nul 2>&1
for %%M in (main cpuid service monitor input processor) do del /q "%HV_STAGE%\Console\%%M.obj" >nul 2>&1
for %%F in (velum.sys velum-console.exe velum.sys.headers.txt velum-console.exe.headers.txt driver.rsp) do del /q "%HV_STAGE%\%%F" >nul 2>&1
for %%D in (Hypervisor Console Temp) do rd "%HV_STAGE%\%%D" 2>nul
rd "%HV_STAGE%" 2>nul
:published
echo Driver:  "%HV_RELEASE%\velum.sys"
echo Console: "%HV_RELEASE%\velum-console.exe"
popd
exit /b 0

:checkDirectory
if not exist "%~1" exit /b 0
for %%P in ("%~1") do set "HV_ATTR=%%~aP"
echo(%HV_ATTR%| findstr /i /c:"l" >nul
if not errorlevel 1 (
    echo ERROR: Refusing a reparse-point build directory: "%~1"
    exit /b 1
)
if not exist "%~1\" exit /b 1
exit /b 0

:verifyPE
findstr /l /c:"8664 machine (x64)" "%~1" >nul || exit /b 1
findstr /l /c:"20B magic # (PE32+)" "%~1" >nul || exit /b 1
if "%HV_C_DEBUG%"=="/Z7" (
    findstr /l /c:"Format: RSDS" "%~1" >nul || exit /b 1
    exit /b 0
)
set "HV_CHECKED="
for /f "tokens=1,2 delims=[] " %%A in ('findstr /l /c:"RVA [size] of Debug Directory" "%~1"') do (
    if not "%%A"=="0" exit /b 1
    if not "%%B"=="0" exit /b 1
    set "HV_CHECKED=1"
)
if not defined HV_CHECKED exit /b 1
for %%K in ("file pointer to symbol table" "number of symbols") do (
    findstr /l /c:"%%~K" "%~1" >nul || exit /b 1
    for /f "tokens=1" %%A in ('findstr /l /c:"%%~K" "%~1"') do if not "%%A"=="0" exit /b 1
)
exit /b 0

:findTools
rem Honor an explicit installation first; otherwise discover VS 2022/2026 editions and Build Tools.
if defined VS_PATH goto :foundVs
set "HV_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%HV_VSWHERE%" set "HV_VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%HV_VSWHERE%" (
    for /f "usebackq tokens=*" %%V in (`call "%HV_VSWHERE%" -latest -products * -version "[17.0,19.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%V"
)
if not defined VS_PATH if defined VSINSTALLDIR set "VS_PATH=%VSINSTALLDIR%"
:foundVs
if not defined VS_PATH exit /b 1
if not exist "%VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" exit /b 1
if not defined WDK_ROOT set "WDK_ROOT=%ProgramFiles(x86)%\Windows Kits\10"
if defined WDK_VER (
    call :kitComplete "%WDK_VER%"
    exit /b
)
rem Current Windows 10/11 kit directory names use fixed-width build numbers.
rem Select the highest complete installed SDK/WDK pair, skipping SDK-only directories.
for /f "delims=" %%K in ('dir /b /ad /o-n "%WDK_ROOT%\Include\10.0.*" 2^>nul') do call :tryKit "%%K"
if not defined WDK_VER exit /b 1
exit /b 0

:tryKit
if defined WDK_VER exit /b 0
call :kitComplete "%~1"
if errorlevel 1 exit /b 0
set "WDK_VER=%~1"
exit /b 0

:kitComplete
for %%H in (km\ntddk.h shared\sdkddkver.h ucrt\stdio.h um\Windows.h) do if not exist "%WDK_ROOT%\Include\%~1\%%H" exit /b 1
for %%L in (km\x64\ntoskrnl.lib km\x64\hal.lib km\x64\BufferOverflowK.lib km\x64\Wdmsec.lib km\x64\Aux_Klib.lib ucrt\x64\libucrt.lib um\x64\kernel32.lib um\x64\advapi32.lib) do if not exist "%WDK_ROOT%\Lib\%~1\%%L" exit /b 1
exit /b 0

:usage
echo Usage: build.bat [release^|diagnostic^|check]
echo Default: optimized Release with no embedded debug directory or PDB output.
popd
exit /b 1

:missingTools
echo ERROR: Install Visual Studio 2022/2026 C++ x64 tools and a complete matching Windows SDK/WDK.
echo You can set VS_PATH, WDK_ROOT and WDK_VER to select an installed toolchain.
:error
echo Build failed. Intermediate files are retained for diagnosis; compilation must succeed before publishing.
popd
exit /b 1
