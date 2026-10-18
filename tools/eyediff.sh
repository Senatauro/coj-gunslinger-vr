#!/bin/sh
# Compara duas capturas BMP do comando SHOT do mod, por exemplo os dois olhos (shot_N_E.bmp e shot_N_D.bmp): conta
# os pixels diferentes (compare -metric AE do ImageMagick, fuzz de 2% por padrão) e, com $3, grava a imagem da diferença.
# Uso: tools/eyediff.sh a.bmp b.bmp [diff.png]. FUZZ=5% muda a tolerância.
t=$(mktemp -d) || exit 1
convert "$1" -alpha off "$t/a.png" && convert "$2" -alpha off "$t/b.png" || exit 1
n=$(compare -metric AE -fuzz "${FUZZ:-2%}" "$t/a.png" "$t/b.png" "${3:-$t/d.png}" 2>&1)
echo "$n pixels diferentes (fuzz ${FUZZ:-2%}) entre $(basename "$1") e $(basename "$2")"
rm -rf "$t"
