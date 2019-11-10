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
 - Use [this branch](https://github.com/daveshah1/yosys/tree/nextpnr_rw_usp) of Yosys **FIXME: upstream Yosys is currently unsupported, even for xc7**

## Prerequisites - Artix-7

 - Run `git submodule init` and `git submodule update` to fetch the database and metadata
 - Download and build [Project Xray](https://github.com/SymbiFlow/prjxray)
 - Use [this branch](https://github.com/daveshah1/yosys/tree/nextpnr_rw_usp) of Yosys **FIXME: upstream Yosys is currently unsupported, even for xc7**

A brief (academic) paper describing the Yosys+nextpnr flow can be found
on [arXiv](https://arxiv.org/abs/1903.10407).

## Building - Artix-7

 - Run `cmake -DARCH=rapidwright .`
 - Run `make` (with -jN as appropriate)

## Building - UltraScale+

 - Run `cmake -DARCH=rapidwright -DRAPIDWRIGHT_PATH=/path/to/rapidwright -DGSON_PATH=/path/to/gson-2.8.5.jar .`
 - Run `make` (with -jN as appropriate)

## Building the Arty example - XRay database
 - Run `pypy3 rapidwright/python/bbaexport.py --device  xc7a35tcsg324-1 --bba rapidwright/xc7a35t.bba`
 - Run `./bbasm rapidwright/xc7a35t.bba rapidwright/xc7a35t.bin`
 - Run `attosoc.sh` in `rapidwright/examples/arty-a35`.


## Creating chip database from RapidWright

 - Run `java -jar rapidwright_bbaexport.jar xczu2cg-sbva484-1-e rapidwright/constids.inc rapidwright/xczu2cg.bba`
   - This uses RapidWright to build a textual representation of a chip database for nextpnr
   - Replace `xczu2cg-sbva484-1-e` and the bba filename with the device you want to target. You can build multiple
     databases for multiple devices if desired (subject to the support caveats above)

 - Run `./bbasm rapidwright/xczu2cg.bba rapidwright/xczu2cg.bin`
   - This converts the text database from above to a binary database that nextpnr can _mmap_
  - See [rapidwright/examples](rapidwright/examples) for example scripts that run the Yosys/nextpnr/RapidWright flow,
    then use Vivado to write a Verilog simulation netlist.

## Notes

  - Currently support:
  - xc7 and xcup: LUTs (including fractured), FFs, DRAM (only RAM64X1D), carry (XORCY and MUXCY), BRAM and IO
  - xcup: OSERDESE3, ISERDESE3, IDDRE1, ODDRE1, IDELAYE3, ODELAYE3, IDELAYCTRL, BUFGCTRL, BUFG, BUFGCE, BUFG_PS, PLLE4_ADV, PLLE4_BASIC, MMCME4_ADV, MMCME4_BASIC
  - xc7: OSERDESE2, ISERDESE2, IDELAYE2, IDELAYCTRL, BUFGCTRL, PLLE2_BASIC, PLLE2_ADV

  - Bels, tile wires and pips are deduplicated but nodes (connections between tile wires) are not. This means
    that databases for larger devices will be several gigabytes in size (but significantly smaller than a fully flat database).
