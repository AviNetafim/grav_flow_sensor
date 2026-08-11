@echo off
setlocal enabledelayedexpansion

echo Disabling auto-connect for all Wi-Fi profiles except ESP32_AP...
echo.

for /f "tokens=2 delims=:" %%A in ('netsh wlan show profiles ^| findstr /C:"All User Profile"') do (
    set "profile=%%A"
    set "profile=!profile:~1!"

    if /I not "!profile!"=="ESP32_AP" (
        echo Setting "!profile!" to manual...
        netsh wlan set profileparameter name="!profile!" connectionmode=manual
    )
)

echo.
echo Setting ESP32_AP to automatic...
netsh wlan set profileparameter name="ESP32_AP" connectionmode=auto

echo.
echo Done.
pause