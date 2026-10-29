#!/bin/zsh
# Regenerates the LVGL fonts in src/fonts from Inter (OFL) and Lucide (ISC).
set -e
cd "$(dirname $0)/fonts"
OUT=../../src/fonts
mkdir -p $OUT
TEXT="0x20-0x7E,0xA0-0xFF,0x2010-0x2027,0x2030-0x203A,0x2190-0x2193,0x20AC,0x2122,0x2212"  # ASCII, Latin-1, punctuation, arrows
ICONS="0xe14d,0xe038,0xe051,0xe060,0xe070,0xe072,0xe073,0xe07b,0xe08b,0xe08c,0xe090,0xe092,0xe0b1,0xe0b2,0xe0b6,0xe0b8,0xe0d0,0xe0dc,0xe0eb,0xe0f0,0xe0fe,0xe105,0xe10f,0xe115,0xe121,0xe122,0xe132,0xe140,0xe141,0xe144,0xe149,0xe14c,0xe157,0xe158,0xe15c,0xe169,0xe16b,0xe17c,0xe186,0xe18e,0xe199,0xe1ae,0xe1af,0xe1b0,0xe1b8,0xe1ba,0xe1bf,0xe214,0xe216,0xe226,0xe284,0xe299,0xe29c,0xe2b1,0xe3c3,0xe415,0xe417"
conv() { npx -y lv_font_conv@1.5.2 --no-compress --bpp 4 --format lvgl "$@"; }

# Text fonts carry the icon set too, at a matching size, so labels can mix " Router".
conv --size 13 --font inter-400.ttf -r $TEXT --font lucide.ttf -r $ICONS -o $OUT/yos_font_13.c
conv --size 15 --font inter-400.ttf -r $TEXT --font lucide.ttf -r $ICONS -o $OUT/yos_font_15.c
conv --size 15 --font inter-600.ttf -r $TEXT --font lucide.ttf -r $ICONS -o $OUT/yos_font_15_semibold.c
conv --size 17 --font inter-400.ttf -r $TEXT --font lucide.ttf -r $ICONS -o $OUT/yos_font_17.c
conv --size 17 --font inter-600.ttf -r $TEXT --font lucide.ttf -r $ICONS -o $OUT/yos_font_17_semibold.c
conv --size 22 --font inter-600.ttf -r $TEXT --font lucide.ttf -r $ICONS -o $OUT/yos_font_22_semibold.c
conv --size 30 --font inter-700.ttf -r $TEXT -o $OUT/yos_font_30_bold.c
# Large numerals for Clock and Weather
conv --size 64 --font inter-400.ttf -r "0x20,0x2D,0x30-0x3A,0xB0,0x25" -o $OUT/yos_font_64.c
conv --size 110 --font inter-400.ttf -r "0x20,0x30-0x3A" -o $OUT/yos_font_110.c
conv --size 88 --font inter-400.ttf -r "0x20,0x2D,0x30-0x3A,0xB0,0x41,0x4D,0x50" -o $OUT/yos_font_88.c
# App icon glyphs
conv --size 36 --font lucide.ttf -r $ICONS -o $OUT/yos_icons_36.c
conv --size 72 --font lucide.ttf -r $ICONS -o $OUT/yos_icons_72.c
