@echo off
setlocal
where cl >nul 2>nul
if not errorlevel 1 goto compiler_ready
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio C++ tools or a Developer Command Prompt are required. & exit /b 1)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (echo Visual Studio C++ tools not found. & exit /b 1)
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
:compiler_ready
pushd "%~dp0.."
if not exist ".local\tests" mkdir ".local\tests"
cl /nologo /EHsc /std:c++17 /I Source\Try_GLB_Put_Into tests\CombatRulesTests.cpp /Fo".local\tests\CombatRulesTests.obj" /Fe".local\tests\CombatRulesTests.exe"
if errorlevel 1 goto failed
".local\tests\CombatRulesTests.exe"
if errorlevel 1 goto failed
cl /nologo /EHsc /std:c++17 /I Source\Try_GLB_Put_Into tests\CityMapProjectionTests.cpp /Fo".local\tests\CityMapProjectionTests.obj" /Fe".local\tests\CityMapProjectionTests.exe"
if errorlevel 1 goto failed
".local\tests\CityMapProjectionTests.exe"
if errorlevel 1 goto failed
popd
exit /b 0
:failed
popd
exit /b 1
