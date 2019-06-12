#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -nobram -top top; write_json blinky.json" ../attosoc/attosoc.v blinky.v
../../../nextpnr-rapidwright --chipdb ../../xczu7ev.bin --json blinky.json --write blinky_routed.json
java -jar ../../../rapidwright_json2dcp.jar xczu7ev-ffvc1156-2-e blinky_routed.json blinky.dcp
vivado -mode batch -nojournal -nolog  -source blinky.tcl
iverilog -o blinky_tb blinky_tb.v blinky_out.v -y${XILINX_VIVADO}/data/verilog/src/unisims ${XILINX_VIVADO}/data/verilog/src/glbl.v
./blinky_tb
