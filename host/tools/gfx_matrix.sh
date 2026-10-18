#!/bin/sh
# Graphics-quality matrix for the THPS1 recomp host (Q18): res-scale x MSAA at 16:9 with 60 Hz
# interpolation, in the Warehouse. One host run per configuration (tools/hfr_probe.py perf + a
# present capture); results in build/hfr/matrix-<time>/<config>/results.json (ROM-derived
# frames: local only). usage: tools/gfx_matrix.sh [SCALES] [MSAAS] [EXTRA HOST ARGS]
set -eu
HERE=${THPS_HOST:?run this through decomp/thps1/recomp-host/tools/gfx_matrix.sh}   # the game's recomp-host (its tools/ has the hfr_probe.py wrapper)
SCALES=${1:-"1 2 3 4"}
MSAAS=${2:-"0 2 4 8"}
EXTRA=${3:-"--refresh 60"}
OUT="$HERE/build/hfr/matrix-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$OUT"
for s in $SCALES; do
  for m in $MSAAS; do
    name="rs${s}_msaa${m}"
    python3 -I "$HERE/tools/hfr_probe.py" --out "$OUT/$name" \
      --args "--aspect 16:9 --window 1920x1080 --res-scale $s --msaa $m --present-size 1920x1080 $EXTRA" \
      perf:8 shots:1:1 > "$OUT/$name.txt" 2>&1 || echo "$name failed" >> "$OUT/failures.txt"
    python3 -I -c "import json,sys; r=json.load(open(sys.argv[1])); p=r.get('0_perf',{}); print(sys.argv[2], {k: p.get(k) for k in ('median_fps','min_fps','presented_per_s','render_ms_per_game_frame','dropped','interpolating','matched','load')})" "$OUT/$name/results.json" "$name" | tee -a "$OUT/summary.txt"
  done
done
echo "$OUT"
