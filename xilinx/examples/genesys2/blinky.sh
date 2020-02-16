#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -abc9 -arch xc7 -top top; write_json blinky.json" blinky.v
../../../nextpnr-xilinx --chipdb ../../xc7k325t.bin --xdc genesys2.xdc --json blinky.json --write blinky_routed.json --fasm blinky.fasm
source "${XRAY_DIR}/utils/environment.sh"
${XRAY_UTILS_DIR}/fasm2frames.py --part xc7k325tffg900-2 --db-root ${XRAY_UTILS_DIR}/../database/kintex7 blinky.fasm > blinky.frames
${XRAY_TOOLS_DIR}/xc7frames2bit --part_file ${XRAY_UTILS_DIR}/../database/kintex7/xc7k325tffg900-2/part.yaml --part_name xc7k325tffg900-2 --frm_file blinky.frames --output_file blinky.bit
