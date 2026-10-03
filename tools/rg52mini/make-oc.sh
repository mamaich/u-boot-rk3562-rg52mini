#!/bin/sh
# Сборка uboot.img с пропатченными таблицами частот BL31 (разгон CPU/GPU).
# Штатная сборка (make.sh без этого скрипта) не затрагивается.
#
#   tools/rg52mini/make-oc.sh                       метки по умолчанию
#   tools/rg52mini/make-oc.sh --cpu 2112:4:4 ...    свои записи (см. bl31_oc.py --help)
#
# Нужен ../rkbin с bin/rk35/rk3562_bl31_v1.21.elf. Патченый ELF кладётся рядом
# под именем rk3562_bl31_v1.21_rg52oc.elf (в rkbin его создаёт только этот скрипт).
set -e
cd "$(dirname "$0")/../.."
RKBIN=../rkbin
SRC=$RKBIN/bin/rk35/rk3562_bl31_v1.21.elf
DST=$RKBIN/bin/rk35/rk3562_bl31_v1.21_rg52oc.elf
if [ $# -gt 0 ]; then ARGS="$*"; else ARGS="--cpu 2112:4:4 --cpu 2208:4:3 --gpu 1000:1:12"; fi
python3 tools/rg52mini/bl31_oc.py "$SRC" -o "$DST" $ARGS
./make.sh rk3562-rg52mini CROSS_COMPILE=${CROSS_COMPILE:-aarch64-none-linux-gnu-} \
          board/rockchip/evb_rk3562/rg52mini-trust-oc.ini
