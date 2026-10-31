#!/bin/bash
# FPGA part of the CI build. Runs inside a Quartus Lite container
# (project root mounted as the working directory).
set -euo pipefail

QV=$(quartus_sh --version | sed -n 's/.*Version \([0-9]*\.[0-9]*\).*/\1/p' | head -1)
echo "Quartus version: ${QV}"

# sys.qsys is saved by Quartus 24.1; point the stock Altera IP versions at
# whatever release is installed in the container.
if [ "${QV}" != "24.1" ]; then
    sed -i "s/version=\"24\.1\"/version=\"${QV}\"/g" sys.qsys
fi

qsys-generate sys.qsys \
    --synthesis=VERILOG \
    --output-directory=sys \
    --family="Cyclone IV E" \
    --part=EP4CE15E22C8 \
    --search-path="$(pwd)/ip/**/*,\$"


# The serial flash interface's 64x1 Avalon-ST FIFO would take a whole M9K
# block, which the downscaler line buffers need: build it from registers.
for f in sys/synthesis/submodules/avst_fifo.v; do
    grep -n "USE_MEMORY_BLOCKS" "$f" || true
    sed -i -E 's/(parameter +USE_MEMORY_BLOCKS *= *)1/\10/' "$f"
    grep -n "USE_MEMORY_BLOCKS" "$f" || true
done

# Fitter seed sweep: stop at the first seed that meets timing.
SEEDS="${SEEDS:-1 2 3 4 5 6}"
met=0
for seed in ${SEEDS}; do
    echo "=== Compiling with SEED=${seed} ==="
    sed -i "s/^set_global_assignment -name SEED .*/set_global_assignment -name SEED ${seed}/" ossc.qsf
    quartus_sh --flow compile ossc
    if ! grep -q "Slack : -" output_files/ossc.sta.summary; then
        echo "=== Timing met with SEED=${seed} ==="
        met=1
        break
    fi
    echo "=== Timing NOT met with SEED=${seed} ==="
    grep -B1 "Slack : -" output_files/ossc.sta.summary || true
done
echo "seed=${seed} timing_met=${met}" > output_files/timing.txt
if [ "${met}" != 1 ]; then
    echo "WARNING: no seed met timing, keeping last result"
fi

# Compressed RBF: needed to fit below the SW image at slot offset 0x50000
quartus_cpf -c -o bitstream_compression=on output_files/ossc.sof output_files/ossc_2nd.rbf
ls -l output_files/ossc_2nd.rbf
