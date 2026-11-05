set -x
make -s -C .. >/dev/null; make -s -B -C . all
for c in balanced max hs-balanced cu+balanced; do ./bench.sh smoke $c; done
T=batch7
TAG=$T ./bench.sh ab stock balanced A 3
TAG=$T ./bench.sh ab stock max A 3
TAG=$T ./bench.sh ab stock hs-balanced A 3
TAG=$T ./bench.sh run hs-max A 2
TAG=$T ./bench.sh run hs-stock A 2
TAG=$T ./bench.sh run stock-script A 2
TAG=$T ./bench.sh ab cu cu+balanced A 2
TAG=$T ./bench.sh run cu-hs+balanced A 2
for c in balanced hs-balanced; do ./fidelity.sh $c; done
TAG=$T ./bench.sh ab stock balanced B 2
TAG=$T ./bench.sh ab stock hs-balanced B 2
TAG=$T ./bench.sh ab cu cu+balanced B 1
./governor.sh balanced A
