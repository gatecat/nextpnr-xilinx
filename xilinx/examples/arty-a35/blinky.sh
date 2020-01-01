#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -nobram -arch xcup -top top; write_json blinky.json" blinky.v
../../../nextpnr-xilinx --chipdb ../../xc7a35t.bin --xdc arty.xdc --json blinky.json --write blinky_routed.json --fasm blinky.fasm
${XRAY_UTILS_DIR}/fasm2frames.py --part xc7a35tcsg324-1 --db-root ${XRAY_UTILS_DIR}/../database/artix7 blinky.fasm > blinky.frames
${XRAY_TOOLS_DIR}/xc7frames2bit --part_file ${XRAY_UTILS_DIR}/../database/artix7/xc7a35tcsg324-1.yaml --part_name xc7a35tcsg324-1  --frm_file blinky.frames --output_file blinky.bit
