#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -nobram -top top; write_json blinky.json" blinky.v
../../../nextpnr-xilinx --chipdb ../../xczu2cg.bin --json blinky.json --write blinky_routed.json
java -jar ../../../rapidwright_json2dcp.jar xczu2cg-sbva484-1-e blinky_routed.json blinky.dcp
vivado -mode batch -nojournal -nolog  -source blinky.tcl
iverilog -o blinky_tb blinky_tb.v blinky_out.v ../sim/*.v -y${XILINX_VIVADO}/data/verilog/src/unisims ${XILINX_VIVADO}/data/verilog/src/glbl.v
./blinky_tb
