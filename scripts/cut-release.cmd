@echo off
REM scripts\cut-release.cmd -- Windows wrapper for scripts\cut-release.sh
REM
REM The one manual step to ship a release, from cmd.exe:
REM
REM     scripts\cut-release.cmd X.Y.Z  [--dry-run] [--no-push] [--allow-dirty]
REM
REM It bumps CMakeLists + CHANGELOG, commits, tags vX.Y.Z, and pushes -- the
REM tag push triggers GitHub Actions, which builds every binary/package and
REM submits to winget/homebrew/scoop/AUR automatically. See cut-release.sh.
REM
REM Runs the POSIX script through Git-Bash (preferred) or WSL. All args pass
REM straight through.

setlocal
set "HERE=%~dp0"
set "SH=%HERE%cut-release.sh"

REM Prefer Git-Bash: native Windows git, no WSL filesystem indirection.
set "GITBASH=%ProgramFiles%\Git\bin\bash.exe"
if exist "%GITBASH%" (
    "%GITBASH%" "%SH%" %*
    exit /b %ERRORLEVEL%
)

set "GITBASH=%ProgramFiles(x86)%\Git\bin\bash.exe"
if exist "%GITBASH%" (
    "%GITBASH%" "%SH%" %*
    exit /b %ERRORLEVEL%
)

REM Fall back to WSL. Translate the script path to a /mnt/... path.
where wsl >nul 2>nul
if %ERRORLEVEL%==0 (
    wsl bash "$(wslpath '%SH%')" %*
    exit /b %ERRORLEVEL%
)

echo cut-release.cmd: no Git-Bash or WSL found to run the release script. 1>&2
echo Install Git for Windows, or run scripts\cut-release.sh from any POSIX shell. 1>&2
exit /b 1
