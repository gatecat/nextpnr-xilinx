#!/usr/bin/env bash
set -ex
yosys -p "synth_xilinx -flatten -arch xcup -nobram -nowidelut -top top; write_json simpleCircuit.json" simpleCircuit.v
../../../nextpnr-xilinx --chipdb ../../xc7k325t.bin --json simpleCircuit.json --xdc Genesys2.xdc --write simpleCircuit_routed.json
python kintex7patch.py simpleCircuit_routed.json
java -jar ../../../rapidwright_json2dcp.jar xc7k325tffg900-2 simpleCircuit_routed.json simpleCircuit.dcp
vivado -mode batch -nojournal -nolog  -source simpleCircuit.tcl
