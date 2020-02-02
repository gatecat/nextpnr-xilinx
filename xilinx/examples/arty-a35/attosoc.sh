#!/usr/bin/env bash
set -ex

yosys -p "synth_xilinx -flatten -nowidelut -abc9 -arch xc7 -top top; write_json attosoc.json" ../attosoc/attosoc.v attosoc_top.v
../../../nextpnr-xilinx --chipdb ../../xc7a35t.bin --xdc arty.xdc --json attosoc.json --write attosoc_routed.json --fasm attosoc.fasm

source "${XRAY_DIR}/utils/environment.sh"

"${XRAY_UTILS_DIR}/fasm2frames.py" --db-root "${XRAY_DATABASE_DIR}/artix7" --part xc7a35tcsg324-1 attosoc.fasm > attosoc.frames
"${XRAY_TOOLS_DIR}/xc7frames2bit" --part_file "${XRAY_DATABASE_DIR}/artix7/xc7a35tcsg324-1/part.yaml" --part_name xc7a35tcsg324-1 --frm_file attosoc.frames --output_file attosoc.bit
