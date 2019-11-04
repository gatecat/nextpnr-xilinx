from xilinx_device import *
from bba import BBAWriter
import sys, argparse
import bels, constid
from nextpnr_structs import *

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--xray", help="Project X-Ray device database path for current family (e.g. ../prjxray-db/artix7)", type=str, required=True)
	parser.add_argument("--metadata", help="nextpnr-xilinx site metadata root", type=str, required=True)
	parser.add_argument("--device", help="name of device to export", type=str, required=True)
	parser.add_argument("--constids", help="name of nextpnr constids file to read", type=str, required=True)
	parser.add_argument("--bba", help="bba file to write", type=str, required=True)
	args = parser.parse_args()
	# Read baked-in constids
	with open(args.constids, "r") as cf:
		constid.read_base(cf)
	# Read and parse X-ray database
	device = import_device(args.device, args.xray, args.metadata)
	# Import tile types
	seen_tiletypes = set()
	for tile in device.tiles:
		if tile.tile_type() not in seen_tiletypes:
			ntt = NextpnrTileType(device, tile)
			seen_tiletypes.add(tile.tile_type())
if __name__ == '__main__':
	main()