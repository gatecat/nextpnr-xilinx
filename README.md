# nextpnr-rapidwright [WIP]

nextpnr is a open-source multi-architecture place-and-route framework
aimed at real-world FPGA silicon. This is an experiment to integrate
nextpnr with RapidWright, an open interface into Xilinx FPGAs.

Currently this project has been designed for UltraScale+ devices only.
Support for UltraScale devices should involve minimal changes, 7-series
support would require significant changes to packing and validity
checking.

## Prerequisites

 - Follow the [RapidWright manual install instructions](https://www.rapidwright.io/docs/Manual_Install.html)
 - Make sure `$RAPIDWRIGHT_PATH` is set correctly for all further steps
 - Download a JAR of recent [GSON](https://repo1.maven.org/maven2/com/google/code/gson/gson/2.8.5/)
 - Use [this branch](https://github.com/daveshah1/yosys/tree/nextpnr_rw_usp) of Yosys

A brief (academic) paper describing the Yosys+nextpnr flow can be found
on [arXiv](https://arxiv.org/abs/1903.10407).

## Building

 - Run `cmake -DARCH=rapidwright -DRAPIDWRIGHT_PATH=/path/to/rapidwright -DGSON_PATH=/path/to/gson-2.8.5.jar .`
 - Run `make` (with -jN as appropriate)

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

  - Currently LUTs (including fractured), FFs, DRAM (only RAM64X1D), and carry (XORCY and MUXCY) are supported.
    There is limited support for manually constrained IO (see the examples) and BUFGCTRLs

  - Bels, tile wires and pips are deduplicated but nodes (connections between tile wires) are not. This means
    that databases for larger devices will be several gigabytes in size.
