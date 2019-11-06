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
	d = import_device(args.device, args.xray, args.metadata)
	# Import tile types
	seen_tiletypes = set()
	tile_types = []
	tile_type_index = {}
	for tile in d.tiles:
		if tile.tile_type() not in seen_tiletypes:
			ntt = NextpnrTileType(d, tile)
			seen_tiletypes.add(tile.tile_type())
			tile_type_index[tile.tile_type()] = len(tile_types)
			tile_types.append(ntt)

	# Import tile instances
	# FIXME: package pins
	tile_insts = []
	for y in range(d.height):
		for x in range(d.width):
			t = d.tiles_by_xy[x, y]
			nti = NextpnrTileInst(index=len(tile_insts), name=t.name,
				tile_type=tile_type_index[t.tile_type()])
			for s in t.sites():
				nsi = NextpnrSiteInst(name=s.name, package_pin="",
					site_xy=s.grid_xy, rel_xy=s.rel_xy(), inter_xy=t.interconn_xy)
				nti.sites.append(nsi)
			nti.tilewire_to_node = [-1] * tile_types[nti.tile_type].tile_wire_count
			tile_insts.append(nti)
if __name__ == '__main__':
	main()
