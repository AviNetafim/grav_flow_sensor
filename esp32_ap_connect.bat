@echo off
setlocal

set "WIFI_IF=Wi-Fi"
set "ESP_SSID=ESP32_AP"

echo Enabling WLAN AutoConfig temporarily...
netsh wlan set autoconfig enabled=yes interface="%WIFI_IF%" >nul

echo Connecting to %ESP_SSID%...
netsh wlan connect name="%ESP_SSID%" interface="%WIFI_IF%"

timeout /t 4 /nobreak >nul

echo Disabling Wi-Fi automatic configuration...
netsh wlan set autoconfig enabled=no interface="%WIFI_IF%"

echo.
echo Current connection:
netsh wlan show interfaces

pause interface="Wi-Fi"