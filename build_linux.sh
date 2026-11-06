#!/bin/bash
set -e

export PICO_SDK_PATH="/mnt/c/Users/Michael/.pico-sdk/sdk/2.3.1"

BUILD_DIR="build_linux"

echo "[PicoA26] Configuring CMake for Waveshare RP2350 Plus 16MB + ILI9341 + MAX98357A..."
cmake -B $BUILD_DIR \
    -DPICO_BOARD=waveshare_rp2350_plus_16mb \
    -DPICO_PLATFORM=rp2350-arm-s \
    -DUSE_ILI9341=1 \
    -DHW_CONFIG=15 \
    -DBUILD_FOR_BOOTLOADER=ON \
    -DCMAKE_BUILD_TYPE=Release

echo "[PicoA26] Compiling PicoA26..."
make -C $BUILD_DIR -j4 PicoA26

if [ -f "$BUILD_DIR/PicoA26.uf2" ]; then
    cp "$BUILD_DIR/PicoA26.uf2" ./PicoA26.uf2
    SIZE=$(stat -c%s "$BUILD_DIR/PicoA26.uf2")
    echo ""
    echo "[SUCESSO] Firmware gerado com sucesso: PicoA26.uf2 ($SIZE bytes)"
    echo "Destino no cartao MicroSD:"
    echo "  -> /emu/PicoA26.uf2 ou /emu/15/PicoA26.uf2"
    echo "  -> Jogos em: roms/a26/"
    echo "  -> Saves em: saves/a26/"
else
    echo "[ERRO] Arquivo PicoA26.uf2 nao foi gerado."
    exit 1
fi
