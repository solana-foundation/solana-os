#!/bin/sh
# Regenerate firmware/main/assets from the brand pack in web/static/brand.
set -e
cd "$(dirname "$0")/.."
B=../web/static/brand
F=$B/Fonts
font() { # file size name fallback range
    bunx --bun lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size "$2" --font "$1" \
        --range "$5" --format lvgl --lv-include lvgl.h ${4:+--lv-fallback $4} \
        --force-fast-kern-format -o "main/assets/$3.c"
}
# ABC Diatype: ASCII, Latin-1 and Latin Extended-A (accented names: SSIDs, owners'
# own Wi-Fi), the bullet. It has no Cyrillic, so that comes from Inter (OFL,
# tools/fonts/, Fontsource 5.3.0's Cyrillic subset) in a font of its own that the
# Diatype one falls back to: its own metrics stay out of Diatype's line heights.
# Then Montserrat, for LV_SYMBOL_* icons.
# lv_font_conv sizes the line from the tallest glyphs (an accented capital: 14 px
# lines became 18), which would move every layout: the Diatype fonts keep the line
# metrics of their ASCII (ascii_lines), and accents rise into a label's extra draw
# area (LVGL's line height / 4).
LAT=0x20-0x7E,0xA0-0x17F,0x2022
ascii_lines() { # file size name
    o=$(mktemp)
    bunx --bun lv_font_conv@1.5.3 --no-compress --no-prefilter --bpp 4 --size "$2" --font "$1" \
        --range 0x20-0x7E --format lvgl --lv-include lvgl.h -o "$o.c"
    for k in line_height base_line; do
        v=$(sed -n "s/^ *\.$k = \(-*[0-9]*\),.*/\1/p" "$o.c")
        sed -i.bak "s/^\( *\.$k = \)-*[0-9]*,/\1$v,/" "main/assets/$3.c"
    done
    rm -f "$o" "$o.c" "main/assets/$3.c.bak"
}
CYR=0x400-0x45F,0x490-0x491
I=tools/fonts
font "$I/inter-cyrillic-400-normal.woff" 12 sol_cyr_12 lv_font_montserrat_12 $CYR
font "$I/inter-cyrillic-400-normal.woff" 14 sol_cyr_14 lv_font_montserrat_14 $CYR
font "$I/inter-cyrillic-500-normal.woff" 20 sol_cyr_20 lv_font_montserrat_20 $CYR
font "$I/inter-cyrillic-700-normal.woff" 32 sol_cyr_32 "" $CYR
font "$F/ABC Diatype Semi-Mono/ABCDiatypeSemi-Mono-Regular.otf" 12 sol_mono_12 sol_cyr_12 $LAT
font "$F/ABC Diatype/ABCDiatype-Regular.otf" 14 sol_font_14 sol_cyr_14 $LAT
font "$F/ABC Diatype/ABCDiatype-Medium.otf" 20 sol_font_20 sol_cyr_20 $LAT
font "$F/ABC Diatype/ABCDiatype-Bold.otf" 32 sol_font_32 sol_cyr_32 $LAT
ascii_lines "$F/ABC Diatype Semi-Mono/ABCDiatypeSemi-Mono-Regular.otf" 12 sol_mono_12
ascii_lines "$F/ABC Diatype/ABCDiatype-Regular.otf" 14 sol_font_14
ascii_lines "$F/ABC Diatype/ABCDiatype-Medium.otf" 20 sol_font_20
ascii_lines "$F/ABC Diatype/ABCDiatype-Bold.otf" 32 sol_font_32
M="$B/Logos/Solana Logomark/2x"
python3 tools/img2lvgl.py "$M/Size=18, Color=color@2x.png" img_logomark_16 main/assets/img_logomark_16.c 16
python3 tools/img2lvgl.py "$M/Size=36, Color=color@2x.png" img_logomark_40 main/assets/img_logomark_40.c 40
python3 tools/img2lvgl.py "$M/Size=72, Color=color@2x.png" img_logomark_72 main/assets/img_logomark_72.c 72
python3 tools/img2lvgl.py "$B/Logos/Solana Foundation Logotype/2x/Size=18, Color=white@2x.png" img_foundation main/assets/img_foundation.c 20
python3 tools/img2lvgl.py "$B/Logos/Solana Foundation Logotype/2x/Size=18, Color=black@2x.png" img_foundation_black main/assets/img_foundation_black.c 20
