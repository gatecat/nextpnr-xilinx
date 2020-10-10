## Building the ultra96 example - RapidWright
 - The first two steps only have to be run once.
 - Run `java -jar rapidwright_bbaexport.jar xczu3eg-sbva484-1-e xilinx/constids.inc xilinx/xczu3eg.bba` from the root of the repo.
 - Run `./bbasm xilinx/xczu3eg.bba xilinx/xczu3eg.bin -b` from the root of the repo.
 - Run `blinky.sh` in `xilinx/examples/ultra96`.

