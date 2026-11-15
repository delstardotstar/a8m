#!/bin/sh
# Writes the A8M EXERCISE folder: tools/exercise.sh OUTDIR [WAVDIR]
# WAVDIR holds Sine.wav, 160-09L.wav and 160-09R.wav (default: A8M TEST in tmp/testcard).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: tools/exercise.sh OUTDIR [WAVDIR]}
wav=${2:-$here/../../tmp/testcard/A8M TEST}
SRC=$here/exercise.c "$here/presets.sh" "$out"
for f in Sine.wav 160-09L.wav 160-09R.wav; do
  cp "$wav/$f" "$out/"
done
cp "$here/exercise-README.txt" "$out/README.txt"
