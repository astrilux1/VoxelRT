#!/bin/bash
# One ~33s top-up pass: adds spp to every scene/state reference accumulation.
# usage: topup.sh <seed>   (use a fresh seed each call)
cd "$(dirname "$0")/.."
S=${1:-999}
for st in pre post; do
  ./bench ref bunker  $st 24 $S >/dev/null 2>&1
  ./bench ref cavern  $st 24 $S >/dev/null 2>&1
  ./bench ref courtyard $st 32 $S >/dev/null 2>&1
  ./bench ref town    $st 32 $S >/dev/null 2>&1
done
# report counts
python3 - <<'EOF'
import struct
for sc in ['bunker','courtyard','cavern','town']:
    for st in ['pre','post']:
        with open(f'results/raw/{sc}_{st}.raw','rb') as f:
            cnt=struct.unpack('d',f.read(8))[0]
        print(f'{sc}_{st}={cnt:.0f}', end=' ')
print()
EOF
