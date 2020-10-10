#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -arch xcup -nobram -top top; write_json blinky.json" zynq_ultra_ps_e_0.v top.v
../../../nextpnr-xilinx --chipdb ../../xczu3eg.bin --json blinky.json --xdc ultra96.xdc --write blinky_routed.json
java -jar ../../../rapidwright_json2dcp.jar xczu3eg-sbva484-1-e blinky_routed.json blinky.dcp
vivado -mode batch -nojournal -nolog  -source blinky.tcl
