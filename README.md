# nextpnr-xilinx

nextpnr is a open-source multi-architecture place-and-route framework
aimed at real-world FPGA silicon. This is an experiment to integrate
nextpnr with RapidWright, an open interface into Xilinx FPGAs, and 
Project Xray, open bitstream documentation for xc7 FPGAs.

Currently two flows are supported:
 - UltraScale+ with RapidWright database generation, bitstream generation
   using RapidWight and Vivado
 - xc7 with Project Xray database generation, bitstream generation
   using FASM and Project Xray (no Vivado anywhere in the flow)

## Prerequisites - UltraScale+

 - Follow the [RapidWright manual install instructions](https://www.rapidwright.io/docs/Manual_Install.html)
 - Make sure `$RAPIDWRIGHT_PATH` is set correctly for all further steps
 - Download a JAR of recent [GSON](https://repo1.maven.org/maven2/com/google/code/gson/gson/2.8.5/)
 - Download and build [Project Xray](https://github.com/SymbiFlow/prjxray)
 - Install [yosys](https://github.com/YosysHQ/yosys)

## Prerequisites - Artix-7

 - Run `git submodule init` and `git submodule update` to fetch the database and metadata
 - Download and build [Project Xray](https://github.com/SymbiFlow/prjxray)
 - Install [yosys](https://github.com/YosysHQ/yosys)

A brief (academic) paper describing the Yosys+nextpnr flow can be found
on [arXiv](https://arxiv.org/abs/1903.10407).

## Building - Artix-7

 - Run `cmake -DARCH=xilinx .`
 - Run `make` (with -jN as appropriate)

## Building - UltraScale+

 - Run `cmake -DARCH=xilinx -DRAPIDWRIGHT_PATH=/path/to/rapidwright -DGSON_PATH=/path/to/gson-2.8.5.jar .`
 - Run `make` (with -jN as appropriate)

## Building the Arty example - XRay database
 - Run `pypy3 xilinx/python/bbaexport.py --device xc7a35tcsg324-1 --bba xilinx/xc7a35t.bba` (regular cpython works as well, but is a lot slower)
 - Run `./bbasm --l xilinx/xc7a35t.bba xilinx/xc7a35t.bin`
 - Set `XRAY_DIR` to the path where Project Xray has been cloned and built (you may also need to patch out the Vivado check for `utils/environment.sh` in Xray by removing this line and everything beyond it: https://github.com/SymbiFlow/prjxray/blob/80726cb73ba5c156549d98a2055f1ee3eff94530/utils/environment.sh#L52)
 - Run `attosoc.sh` in `xilinx/examples/arty-a35`.

## Building the zcu104 example - RapidWright
 - Run `java -jar rapidwright_bbaexport.jar xczu7ev-ffvc1156-2-e xilinx/constids.inc xilinx/xczu7ev.bba`
 - Run `./bbasm --l xilinx/xczu7ev.bba xilinx/xczu7ev.bin`
 - Run `blinky.sh` in `xilinx/examples/zcu104`.

## Creating chip database from RapidWright

 - Run `java -jar rapidwright_bbaexport.jar xczu2cg-sbva484-1-e xilinx/constids.inc xilinx/xczu2cg.bba`
   - This uses RapidWright to build a textual representation of a chip database for nextpnr
   - Replace `xczu2cg-sbva484-1-e` and the bba filename with the device you want to target. You can build multiple
     databases for multiple devices if desired (subject to the support caveats above)

 - Run `./bbasm --l xilinx/xczu2cg.bba xilinx/xczu2cg.bin`
   - This converts the text database from above to a binary database that nextpnr can _mmap_
  - See [xilinx/examples](xilinx/examples) for example scripts that run the Yosys/nextpnr/RapidWright flow,
    then use Vivado to write a Verilog simulation netlist.

## Notes

  - Currently supported:
  - xc7 and xcup: LUTs (including fractured), FFs, DRAM (only RAM64X1D), carry (XORCY and MUXCY or CARRY4), SRL16E and SRLC32E (no cascading), BRAM and IO
  - xcup: OSERDESE3, ISERDESE3, IDDRE1, ODDRE1, IDELAYE3, ODELAYE3, IDELAYCTRL, BUFGCTRL, BUFG, BUFGCE, BUFG_PS, PLLE4_ADV, PLLE4_BASIC, MMCME4_ADV, MMCME4_BASIC, URAM288E, DSP48E2 (no cascading)
  - xc7: OSERDESE2, ISERDESE2, IDELAYE2, IDELAYCTRL, BUFGCTRL, PLLE2_BASIC, PLLE2_ADV

  - Bels, tile wires and pips are deduplicated but nodes (connections between tile wires) are not. This means
    that databases for larger devices will be several gigabytes in size (but significantly smaller than a fully flat database).
