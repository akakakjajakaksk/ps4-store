#!/usr/bin/env sh
set -eu
ROOT="${1:-pkgroot}"
fail=0
for f in "eboot.bin" "sce_sys/param.sfo" "sce_sys/icon0.png"; do
  if [ ! -f "$ROOT/$f" ]; then echo "FALTA: $ROOT/$f"; fail=1; else echo "OK: $ROOT/$f"; fi
done
if [ "$fail" -ne 0 ]; then
  echo "Pacote incompleto. Nao gere/renomeie um arquivo como .pkg."
  exit 1
fi
echo "Layout minimo pronto para o empacotador PS4."
