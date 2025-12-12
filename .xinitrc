#!/bin/bash
# ~/.xinitrc

# disable blanking / sleep
xset s off
xset -dpms
xset s noblank
openbox &
# Log X environment info before running app
{
  echo "==== $(date) ===="
  echo "DISPLAY=$DISPLAY"
  echo "XDG_SESSION_TYPE=$XDG_SESSION_TYPE"
  xrandr --query
  xdpyinfo | grep dimensions
} > /home/pi/xsession.log 2>&1
xrandr --output HDMI-1 --mode 1920x1080 --rate 60 2>>/home/pi/xsession.log
cd /home/pi/KIOSK
/home/pi/KIOSK/token_display >> /home/pi/app.log 2>&1 &
sleep 3
wmctrl -r :ACTIVE: -b add, fullscreen
wait
