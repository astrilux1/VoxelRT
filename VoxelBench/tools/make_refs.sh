#!/bin/bash
# accumulate path-traced references for all scenes/states. usage: make_refs.sh <spp> <seed>
cd "$(dirname "$0")/.."
SPP=${1:-64}; SEED=${2:-0}
for sc in bunker courtyard cavern town; do
  for st in pre post; do
    ./bench ref $sc $st $SPP $SEED
  done
done
echo "refs done spp=$SPP seed=$SEED" >> results/refs.log
