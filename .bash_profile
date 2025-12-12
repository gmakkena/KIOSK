#!/bin/bash
# Auto-start X on TTY1 after login
if [[ -z $DISPLAY ]] && [[ $(tty) == /dev/tty1 ]]; then
 # echo "Starting X session for kiosk..."
  startx -- -nocursor>/dev/null 2>&1
#startx /usr/bin/env bash -c "cd /home/pi/KIOSK && ./token_display > /home/pi/kiosk_debug.log 2>&1"
# startx /usr/bin/env bash -c 'cd /home/pi/KIOSK && ./token_display > /home/pi/kiosk_debug.log 2>&1' -- -nocursor >/dev/null 2>&1
  logout
fi
