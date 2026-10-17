#!/bin/sh
# Замер движка overCraft на основной машине (Linux), без overOS и QEMU.
#
# Собирает настоящие renderer.cpp, генератор и сборщик сеток с заглушкой
# ovos.h (рабочих ядер нет — всё на одном потоке), строит мир 13x13 чанков,
# ставит камеру туда же, куда игра после первых шагов, и печатает время
# геометрии и растеризации за кадр (среднее за 60 кадров).
#
#   tools/ovcraft_bench/run.sh [ширина высота [кадр.ppm [процент]]]
#   tools/ovcraft_bench/run.sh models кадр.ppm   — модели, предметы и рука
#   tools/ovcraft_bench/run.sh sim [кадров]       — вся игра по сценарию под ASan
#   tools/ovcraft_bench/run.sh mem                — куча движка (код 12) при смене размера окна
#     (SCENE=models|hand|held|item, CAM=x,y,z, YAW, PITCH, EYAW, ANIM, SWING)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
G="$HERE/../../src/files/progs/overCraft"
OUT="${TMPDIR:-/tmp}/ovcraft_bench"
if [ "$1" = mem ]; then
    # Куча движка под настоящим cxxrt с ареной из makefile игры
    ARENA=$(awk '/^LL_ARENA_KB/ {print $3}' "$G/makefile")
    sed '/__dso_handle/d' "$HERE/../../src/libs/cxxrt/cxxrt.cpp" > "$OUT.cxxrt.cpp"
    g++ -std=c++17 -O1 -fno-exceptions -fno-rtti -fno-builtin -DCXXRT_ARENA_KB="$ARENA" -I "$HERE/fake" -I "$G" -I "$G/engine" \
        -I "$HERE/../../src/libs" -I "$HERE/../../src/libs/cxxrt" -o "$OUT.mem" "$HERE/memsim.cpp" "$G/engine/renderer.cpp" \
        "$G/engine/jobs.cpp" "$OUT.cxxrt.cpp" 2>&1 | grep -v warning || true
    exec "$OUT.mem"
fi
if [ "$1" = sim ]; then
    shift
    g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -DOE_DEBUG -fno-exceptions -fno-rtti \
        -I "$HERE/fake" -I "$G" -I "$G/engine" -I "$HERE/../../src/libs" -DOVCRAFT_DIR="\"$G\"" -o "$OUT.sim" \
        "$HERE/sim.cpp" "$G/engine/renderer.cpp" "$G/engine/jobs.cpp" "$G/world/generator.cpp" "$G/world/mesher.cpp" \
        "$G/world/world.cpp" "$G/game/player.cpp" "$G/game/physics.cpp" "$G/game/entity.cpp" "$G/game/model.cpp" \
        "$G/game/shapes.cpp" "$G/game/hand.cpp" "$G/game/inventory.cpp" "$G/game/icons.cpp"
    exec "$OUT.sim" "$@"
fi
if [ "$1" = models ]; then
    shift
    g++ -std=c++17 -O1 -fno-exceptions -fno-rtti -I "$HERE/fake" -I "$G" -I "$G/engine" -I "$HERE/../../src/libs" -DOVCRAFT_DIR="\"$G\"" -o "$OUT.models" \
        "$HERE/models.cpp" "$G/engine/renderer.cpp" "$G/engine/jobs.cpp" "$G/game/model.cpp" "$G/game/shapes.cpp" "$G/game/hand.cpp"
    exec "$OUT.models" "$@"
fi
g++ -std=c++17 -O2 -fno-exceptions -fno-rtti -I "$HERE/fake" -I "$G" -I "$G/engine" -I "$HERE/../../src/libs" -DOVCRAFT_DIR="\"$G\"" -o "$OUT" \
    "$HERE/bench.cpp" "$G/engine/renderer.cpp" "$G/engine/jobs.cpp" "$G/world/generator.cpp" "$G/world/mesher.cpp"
"$OUT" "$@"
