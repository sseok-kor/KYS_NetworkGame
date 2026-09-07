@echo off
REM Crash minidump auto-analysis helper.
REM Usage: analyze_dump.cmd Dumps\ServerApp_YYYYMMDD_HHMMSS_pid1234.dmp
REM Requires cdb on PATH (Windows SDK - Debugging Tools for Windows).
REM Our own ServerApp.pdb/LoginServer.pdb are found via the build path recorded in the dump,
REM so keep the crash-time exe+pdb pair (GUID match); a rebuilt pdb will not resolve symbols.

if "%~1"=="" (
  echo usage: analyze_dump.cmd ^<dumpfile.dmp^>
  exit /b 1
)

REM OS symbols (ntdll/kernel32) from the Microsoft symbol server, cached under %TEMP%\symbols.
if not defined _NT_SYMBOL_PATH set "_NT_SYMBOL_PATH=srv*%TEMP%\symbols*https://msdl.microsoft.com/download/symbols"

cdb -z "%~1" -c "!analyze -v; ~*kb; q"
