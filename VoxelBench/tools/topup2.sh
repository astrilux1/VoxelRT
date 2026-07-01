#!/bin/bash
# ~35s top-up pass weighted toward high-variance scenes (bunker, cavern).
# usage: topup2.sh <seed>
cd "$(dirname "$0")/.."
S=${1:-999}
for st in pre post; do
  ./bench ref bunker $st 40 $S >/dev/null 2>&1
  ./bench ref cavern $st 40 $S >/dev/null 2>&1
done
python3 - <<'EOF'
import struct
for sc in ['bunker','courtyard','cavern','town']:
    for st in ['pre','post']:
        cnt=struct.unpack('d',open(f'results/raw/{sc}_{st}.raw','rb').read(8))[0]
        print(f'{sc}_{st}={cnt:.0f}', end=' ')
print()
EOF
