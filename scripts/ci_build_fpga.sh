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

# Same tweak as scripts/qsys.patch (JTAG master FIFO depth), tolerant of
# line-number differences between Quartus versions.
sed -i 's/\.FIFO_DEPTH          (64),/.FIFO_DEPTH          (1024),/' sys/synthesis/submodules/sys_master_0.v || true

quartus_sh --flow compile ossc

# Compressed RBF: needed to fit below the SW image at slot offset 0x50000
quartus_cpf -c -o bitstream_compression=on output_files/ossc.sof output_files/ossc_2nd.rbf
ls -l output_files/ossc_2nd.rbf
