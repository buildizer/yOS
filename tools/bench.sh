#!/bin/zsh
# Measures frame rate on the board during three animations. Needs YOS_DEV_TOOLS and the home screen showing.
H=http://192.168.88.164
t() { curl -s -m 5 -o /dev/null "$H/api/touch?$1"; }
perf() { curl -s -m 5 "$H/api/perf"; }
t "x=240&y=400"; sleep 1.5                       # wake / no-op on empty home area
perf >/dev/null
t "x=60&y=215"; sleep 0.6; echo "open app:    $(perf)"   # opens Settings (rise + fade)
sleep 0.5; perf >/dev/null
t "x=200&y=179"; sleep 0.6; echo "page slide:  $(perf)"   # Display & Brightness slides in
sleep 0.5; perf >/dev/null
t "x=240&y=400&x2=240&y2=120&ms=300"; sleep 0.6; echo "scroll:      $(perf)"
sleep 0.5; perf >/dev/null
t "x=240&y=470&x2=240&y2=330&ms=250"; sleep 0.8; echo "go home:     $(perf)"
