set -x
T=batch8
TAG=$T ./bench.sh ab stock balanced B 2
TAG=$T ./bench.sh run max B 2
TAG=$T ./bench.sh run hs-stock B 1
./governor.sh hs-balanced A
