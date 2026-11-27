@echo off
setlocal
set "launcher=%~dp0WrathFall.exe"
if not exist "%launcher%" set "launcher=%~dp0..\WrathFall.exe"
if not exist "%launcher%" (
    echo Extract the complete WrathFall download first.
    exit /b 1
)
"%launcher%" --collect
set "result=%errorlevel%"
if not "%result%"=="0" pause
exit /b %result%
