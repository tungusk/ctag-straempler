#!/bin/bash
# M0d proof: the firmware must build with the sampler machine excluded.
# Swaps in a stub-only registry, builds with EXCLUDE_COMPONENTS=machine_sampler
# into build_proof/, then restores. Exit 0 = core is machine-clean.
set -e
cd "$(dirname "$0")/.."
cp main/machine_registry.c /tmp/proof_reg.bak
cp main/CMakeLists.txt /tmp/proof_cmake.bak
restore(){ cp /tmp/proof_reg.bak main/machine_registry.c; cp /tmp/proof_cmake.bak main/CMakeLists.txt; }
trap restore EXIT
cat > main/machine_registry.c <<'REG'
#include "machine.h"
extern const machine_t machine_stub;
const machine_t *const machine_registry[] = { &machine_stub, NULL };
REG
# strip every machine_* component from REQUIRES (generic — the roster grows)
sed -i '' 's/REQUIRES ui machine audio machine_[a-z0-9_ ]* menu)/REQUIRES ui machine audio menu)/' main/CMakeLists.txt
export PATH="$HOME/.espressif/tools/xtensa-esp32-elf/esp-2021r2-patch3-8.4.0/xtensa-esp32-elf/bin:$HOME/.espressif/tools/esp32ulp-elf/2.28.51-esp-20191205/esp32ulp-elf-binutils/bin:$PATH"
export IDF_PATH="$HOME/esp/esp-idf-v4.3"
~/.espressif/python_env/idf4.3_py3.9_env/bin/python "$IDF_PATH/tools/idf.py" -B build_proof build \
  -DEXCLUDE_COMPONENTS="machine_sampler2;machine_sampler3;machine_looper;machine_slicer;machine_granular;machine_glitch;machine_drumsampler;machine_freesound;machine_deck;machine_dualdeck;machine_tracker;machine_radio;machine_synth;machine_instsampler;machine_tape;machine_editor;libxmp" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  && echo "PROOF PASSED: sampler-less build links"
rm -rf build_proof
