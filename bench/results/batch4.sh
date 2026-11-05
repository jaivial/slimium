set -x
for c in balanced hs-balanced cu+balanced max hs-max cu-hs+balanced; do ./fidelity.sh $c; done
for c in stock hs-stock hs-max cu-hs+balanced cu; do ./bench.sh smoke $c; done
T=batch4
TAG=$T ./bench.sh ab stock hs-max A 3
TAG=$T ./bench.sh ab cu cu-hs+balanced A 3
TAG=$T ./bench.sh run cu-hs A 2
./governor.sh hs-balanced A
TAG=$T ./bench.sh ab stock balanced B 2
TAG=$T ./bench.sh ab stock hs-balanced B 2
TAG=$T ./bench.sh run cu B 2
TAG=$T ./bench.sh run cu-hs+balanced B 2
