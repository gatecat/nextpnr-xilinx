#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -arch xcup -nobram -top top; write_json blinky.json" ../attosoc/attosoc.v blinky.v
../../../nextpnr-xilinx --chipdb ../../xczu7ev.bin --json blinky.json --xdc zcu104.xdc --write blinky_routed.json
java -jar ../../../rapidwright_json2dcp.jar xczu7ev-ffvc1156-2-e blinky_routed.json blinky.dcp
vivado -mode batch -nojournal -nolog  -source blinky.tcl
