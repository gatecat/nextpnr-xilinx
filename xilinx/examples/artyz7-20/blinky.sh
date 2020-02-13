#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -abc9 -arch xc7 -top top; write_json blinky.json" blinky.v
../../../nextpnr-xilinx --chipdb ../../xc7z020.bin --xdc artyz7.xdc --json blinky.json --write blinky_routed.json --fasm blinky.fasm
source "${XRAY_DIR}/utils/environment.sh"
${XRAY_UTILS_DIR}/fasm2frames.py --part xc7z020clg400-1 --db-root ${XRAY_UTILS_DIR}/../database/zynq7 blinky.fasm > blinky.frames
${XRAY_TOOLS_DIR}/xc7frames2bit --part_file ${XRAY_UTILS_DIR}/../database/zynq7/xc7z020clg400-1/part.yaml --part_name xc7z020clg400-1 --frm_file blinky.frames --output_file blinky.bit
