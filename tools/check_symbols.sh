#!/bin/sh
# Prüft, dass das Plugin keine glibc-Symbole neuer als $2 braucht.
so="$1"; max="$2"
bad=$(objdump -T "$so" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -Vu |
      awk -v m="$max" '{ split($0,a,"."); split(m,b,"."); if (a[1]>b[1] || (a[1]==b[1] && a[2]>b[2])) print }')
if [ -n "$bad" ]; then
	echo "FEHLER: $so braucht zu neue glibc-Symbole: $bad" >&2
	objdump -T "$so" | grep -E "GLIBC_($(echo $bad | tr ' ' '|'))" >&2
	rm -f "$so"
	exit 1
fi
echo "glibc-Symbole ok (<= $max)"
