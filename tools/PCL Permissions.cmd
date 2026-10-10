@echo off
setlocal
set "PCL_POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if exist "%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe" set "PCL_POWERSHELL=%SystemRoot%\Sysnative\WindowsPowerShell\v1.0\powershell.exe"
"%PCL_POWERSHELL%" -NoProfile -STA -File "%~dp0pcl_permissions_gui.ps1"
if errorlevel 1 (
    echo PCL permissions setup could not complete. Review the error above.
    echo Windows script execution policy is not changed by this launcher.
    pause
)
endlocal