# Heater - Build and upload to latest GitHub release
param(
	[string]$Environment = "esp32c3",
	[string]$Repo = "tarcsajana/Heater"
)

$ErrorActionPreference = "Stop"

$platformIoCmd = Get-Command platformio -ErrorAction SilentlyContinue
if (-not $platformIoCmd) {
	$platformIoCmd = Get-Command pio -ErrorAction SilentlyContinue
}
if (-not $platformIoCmd) {
	$fallbackPio = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\platformio.exe"
	if (Test-Path $fallbackPio) {
		$platformIoCmd = @{ Path = $fallbackPio }
	}
}
if (-not $platformIoCmd) {
	throw "PlatformIO CLI not found. Install PlatformIO Core or add it to PATH."
}
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
	throw "GitHub CLI (gh) not found in PATH."
}

Push-Location $PSScriptRoot
try {
	Write-Host "Building Heater for environment '$Environment'..." -ForegroundColor Green
	& $platformIoCmd.Path run -e $Environment

	$tag = gh release list --repo $Repo --limit 1 --exclude-drafts --json tagName --jq ".[0].tagName"
	if ([string]::IsNullOrWhiteSpace($tag)) {
		throw "No release tag found in repository '$Repo'."
	}

	$firmwarePath = ".pio/build/$Environment/firmware.bin"
	if (-not (Test-Path $firmwarePath)) {
		throw "Firmware not found at '$firmwarePath'. Build may have failed."
	}

	Write-Host "Uploading firmware to release: $tag" -ForegroundColor Green
	gh release upload $tag "$firmwarePath#firmware.bin" --repo $Repo --clobber

	Write-Host "Upload completed." -ForegroundColor Green
}
finally {
	Pop-Location
}
