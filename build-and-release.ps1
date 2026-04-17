# Heater - Build and upload to GitHub release
Push-Location "d:\OneDrive\Dokumentumok\PlatformIO\Projects\Heater"
Write-Host "Building Heater..." -ForegroundColor Green
C:\Users\tarcs\.platformio\penv\Scripts\platformio.exe run -e esp32c3
$tag = gh release list --repo tarcsajana/Heater --limit 1 --exclude-drafts --json tagName --jq ".[0].tagName"
Write-Host "Uploading firmware to release: $tag" -ForegroundColor Green
gh release upload $tag .pio/build/esp32c3/firmware.bin#firmware.bin --repo tarcsajana/Heater --clobber
Pop-Location
