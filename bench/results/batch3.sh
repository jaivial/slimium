set -x
T=batch3
TAG=$T ./bench.sh ab stock balanced A 3
TAG=$T ./bench.sh ab stock hs-balanced A 3
TAG=$T ./bench.sh run hs-stock A 3
TAG=$T ./bench.sh ab cu cu+balanced A 3
TAG=$T ./bench.sh run configs/explore/cu-nodgpu+balanced A 3
TAG=$T ./bench.sh run max A 3
TAG=$T ./bench.sh run configs/explore/p1+ipgpu A 2
TAG=$T ./bench.sh run configs/explore/p1+dgpu A 2
./governor.sh balanced A
for c in balanced hs-balanced cu+balanced max; do ./fidelity.sh $c; done
for c in balanced max cu+balanced hs-balanced; do ./bench.sh smoke $c; done
