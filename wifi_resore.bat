@echo off
setlocal

set "WIFI_IF=Wi-Fi"

echo Enabling Wi-Fi automatic configuration...
netsh wlan set autoconfig enabled=yes interface="%WIFI_IF%"

echo Disconnecting current Wi-Fi connection...
netsh wlan disconnect interface="%WIFI_IF%" >nul

timeout /t 2 /nobreak >nul

echo Windows will now reconnect using its preferred Wi-Fi profile order.

timeout /t 5 /nobreak >nul

echo.
echo Current connection:
netsh wlan show interfaces

pause