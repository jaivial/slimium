set -x
make -s -C .. >/dev/null; make -s -B -C . all
T=batch5
TAG=$T ./bench.sh ab stock balanced A 3
TAG=$T ./bench.sh ab stock max A 3
TAG=$T ./bench.sh ab stock hs-balanced A 3
TAG=$T ./bench.sh run hs-max A 3
TAG=$T ./bench.sh ab cu cu+balanced A 2
TAG=$T ./bench.sh run cu-hs+balanced A 2
for c in balanced max hs-balanced; do ./fidelity.sh $c; done
for c in balanced max hs-balanced cu+balanced cu-hs+balanced stock; do ./bench.sh smoke $c; done
TAG=$T ./bench.sh ab stock balanced B 2
TAG=$T ./bench.sh ab stock hs-balanced B 2
./governor.sh balanced A
./governor.sh hs-balanced A
