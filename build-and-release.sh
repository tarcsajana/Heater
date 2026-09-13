#!/usr/bin/env sh
set -eu

ENVIRONMENT="esp32c3"
REPO="tarcsajana/Heater"

usage() {
  cat <<'EOF'
Usage: ./build-and-release.sh [-e environment] [-r owner/repo]

Options:
  -e, --environment   PlatformIO environment (default: esp32c3)
  -r, --repo          GitHub repository in owner/name format (default: tarcsajana/Heater)
  -h, --help          Show this help
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    -e|--environment)
      ENVIRONMENT="${2:-}"
      shift 2
      ;;
    -r|--repo)
      REPO="${2:-}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [ -z "$ENVIRONMENT" ]; then
  echo "Environment cannot be empty." >&2
  exit 1
fi

if [ -z "$REPO" ]; then
  echo "Repo cannot be empty." >&2
  exit 1
fi

if command -v platformio >/dev/null 2>&1; then
  PLATFORMIO_CMD="platformio"
elif command -v pio >/dev/null 2>&1; then
  PLATFORMIO_CMD="pio"
elif [ -x "$HOME/.platformio/penv/bin/platformio" ]; then
  PLATFORMIO_CMD="$HOME/.platformio/penv/bin/platformio"
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PLATFORMIO_CMD="$HOME/.platformio/penv/bin/pio"
else
  echo "PlatformIO CLI not found. Install PlatformIO Core or add it to PATH." >&2
  exit 1
fi

if command -v gh >/dev/null 2>&1; then
  GH_CMD="gh"
elif [ -x "/snap/bin/gh" ]; then
  GH_CMD="/snap/bin/gh"
elif [ -x "/usr/bin/gh" ]; then
  GH_CMD="/usr/bin/gh"
elif [ -x "/usr/local/bin/gh" ]; then
  GH_CMD="/usr/local/bin/gh"
else
  echo "GitHub CLI (gh) not found in PATH." >&2
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "Building Heater for environment '$ENVIRONMENT'..."
"$PLATFORMIO_CMD" run -e "$ENVIRONMENT"

tag="$("$GH_CMD" release list --repo "$REPO" --limit 1 --exclude-drafts --json tagName --jq '.[0].tagName')"
if [ -z "$tag" ] || [ "$tag" = "null" ]; then
  echo "No release tag found in repository '$REPO'." >&2
  exit 1
fi

firmware_path=".pio/build/$ENVIRONMENT/firmware.bin"
if [ ! -f "$firmware_path" ]; then
  echo "Firmware not found at '$firmware_path'. Build may have failed." >&2
  exit 1
fi

echo "Uploading firmware to release: $tag"
"$GH_CMD" release upload "$tag" "$firmware_path#firmware.bin" --repo "$REPO" --clobber

echo "Upload completed."
