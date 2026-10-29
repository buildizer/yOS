#!/bin/zsh
# Build and flash yOS.
#   tools/flash.sh          asks which way (shows which ones are available right now)
#   tools/flash.sh usb      over the USB cable; yOS installs it and shows the update screen
#   tools/flash.sh ota      over Wi-Fi (the board and this Mac must be on the same network)
#   tools/flash.sh recovery over USB through the chip's ROM loader: no screen, but works even
#                           when the installed firmware is broken
#   tools/flash.sh auto     Wi-Fi when the board answers, else USB, without asking
# YOS_HOST overrides the board's Wi-Fi address (default 192.168.88.164).
set -e
setopt null_glob  # an unmatched port pattern is empty, not an error
cd "$(dirname $0)/.."
MODE=${1:-ask}
HOST=${YOS_HOST:-192.168.88.164}
PASS=$(sed -n 's/^#define OTA_PASS "\(.*\)"/\1/p' include/secrets.h)

reachable() { curl -s -m 3 -o /dev/null "http://$HOST/" ; }
usb_port() { ls /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1; }  # changes with the USB socket used

if [[ $MODE == ask ]]; then
  PORT=$(usb_port)
  if [[ -n $PORT ]]; then USB_STATE="connected ($PORT)"; else USB_STATE="no cable found"; fi
  if reachable; then WIFI_STATE="board answers at $HOST"; else WIFI_STATE="board not reachable at $HOST"; fi
  echo "How do you want to flash yOS?"
  echo "  1) USB cable     ($USB_STATE) - shows the update screen"
  echo "  2) Wi-Fi (OTA)   ($WIFI_STATE) - shows the update screen"
  echo "  3) USB recovery  ($USB_STATE) - no screen; use if the board won't start"
  read "choice?Choose 1, 2 or 3: "
  case $choice in
    1) MODE=usb ;;
    2) MODE=ota ;;
    3) MODE=recovery ;;
    *) echo "Cancelled." >&2; exit 2 ;;
  esac
elif [[ $MODE == auto ]]; then
  if reachable; then MODE=ota
  elif [[ -n $(usb_port) ]]; then MODE=usb
  else echo "Board not reachable on Wi-Fi ($HOST) and no USB serial port found." >&2; exit 1
  fi
  echo "auto: using $MODE"
fi

START=$(date +%s)
case $MODE in
  ota)
    [[ -n $PASS ]] || { echo "OTA_PASS missing from include/secrets.h" >&2; exit 1; }
    reachable || { echo "The board doesn't answer at $HOST. Is this Mac on the same Wi-Fi, and is Wi-Fi on in the board's Settings?" >&2; exit 1; }
    pio run -e yos  # one build for every route; espota sends that same file
    PY=$(ls ~/.platformio/penv/bin/python /opt/homebrew/opt/platformio/libexec/bin/python 2>/dev/null | head -1)
    "$PY" ~/.platformio/packages/framework-arduinoespressif32/tools/espota.py -i "$HOST" -p 3232 \
      -a "$PASS" -f .pio/build/yos/firmware.bin ;;
  usb)
    PORT=$(usb_port)
    [[ -n $PORT ]] || { echo "No USB serial port found. Check the cable." >&2; exit 1; }
    pio run -e yos
    echo "Sending to yOS on $PORT"
    PY=$(ls ~/.platformio/penv/bin/python /opt/homebrew/opt/platformio/libexec/bin/python 2>/dev/null | head -1)
    "$PY" tools/usb_update.py .pio/build/yos/firmware.bin "$PORT" ;;
  recovery)
    PORT=$(usb_port)
    [[ -n $PORT ]] || { echo "No USB serial port found. Check the cable." >&2; exit 1; }
    echo "USB port: $PORT (ROM loader, the screen stays dark until it restarts)"
    pio run -e yos -t upload --upload-port "$PORT" ;;
  *)
    echo "usage: tools/flash.sh [usb|ota|recovery|auto]   (no argument: asks)" >&2; exit 2 ;;
esac
echo "Flashed over $MODE in $(( $(date +%s) - START )) s"
