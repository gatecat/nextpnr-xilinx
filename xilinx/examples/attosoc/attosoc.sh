#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -nobram -top top; write_json attosoc.json" attosoc_top.v attosoc.v
../../../nextpnr-xilinx --chipdb ../../xczu2cg.bin --json attosoc.json --write attosoc_routed.json
java -jar ../../../rapidwright_json2dcp.jar xczu2cg-sbva484-1-e attosoc_routed.json attosoc.dcp
vivado -mode batch -nojournal -nolog  -source attosoc.tcl
iverilog -o attosoc_tb attosoc_tb.v attosoc_out.v  ../sim/*.v -y${XILINX_VIVADO}/data/verilog/src/unisims ${XILINX_VIVADO}/data/verilog/src/glbl.v
./attosoc_tb
