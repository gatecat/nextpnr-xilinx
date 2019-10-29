import json
import os
from gridinfo import parse_gridinfo
# Represents Xilinx device data from PrjXray etc

class WireData:
	def __init__(self, index, name, tied_value=None):
		self.index = index
		self.name = name
		self.tied_value = tied_value

class PIPData:
	def __init__(self, index, from_wire, to_wire, is_route_thru):
		self.index = index
		self.from_wire = from_wire
		self.to_wire = to_wire
		self.is_route_thru = is_route_thru

class SiteWireData:
	def __init__(self, name, is_pin=False, tile_wire_index=None):
		self.name = name
		self.is_pin = is_pin
		self.tile_wire_index = tile_wire_index

class SiteBELPinData:
	def __init__(self, name, pindir, site_wire_idx):
		self.name = name
		self.pindir = pindir
		self.site_wire_index = site_wire_index

class SiteBELData:
	def __init__(self, name, bel_type, pins):
		self.name = name
		self.bel_type = bel_type
		self.pins = pins

class SitePIPData:
	def __init__(self, bel_idx, bel_input_idx, from_wire_idx, to_wire_idx):
		self.bel_idx = bel_idx
		self.bel_input_idx = bel_input_idx
		self.from_wire_idx = from_wire_idx
		self.to_wire_idx  = to_wire_idx

class SiteData:
	def __init__(self, site_type):
		self.site_type = site_type
		self.wires = []
		self.bels = []
		self.pips = []
		self.pins = []
		self.variants = {}

class TileData:
	def __init__(self, tile_type):
		self.tile_type = tile_type
		self.wires = []
		self.pips = []

class PIP:
	def __init__(self, tile, index):
		self.tile = tile
		self.index = index
		self.data = tile.get_pip_data(index)
	def srcWire(self):
		return Wire(self.tile, self.data.from_wire)
	def dstWire(self):
		return Wire(self.tile, self.data.to_wire)
	def isRouteThru(self):
		return self.data.is_route_thru

class Wire:
	def __init__(self, tile, index):
		self.tile = tile
		self.index = index
		self.data = tile.get_wire_data(index)
	def name(self):
		return self.data.name
	def node(self):
		return self.tile.wire_to_node(index)

class SiteBEL:
	def __init__(self, site, index):
		self.site = site
		self.index = index
		self.data = site.get_bel_data(index)

class Site:
	def __init__(self, tile, name, grid_xy, data):
		self.tile = tile 
		self.name = name
		self.grid_xy = grid_xy
		self.data = data

	def bels(self):
		return (SiteBEL(self, i) for i in range(len(self.data.bels)))
	def available_variants(self):
		return self.data.variants.keys()
	def variant(self, vtype):
		vsite = Site(self.tile, self.name, self.grid_xy, self.data.variants[vtype])
		return vsite

class Tile:
	def __init__(self, x, y, name, data, site_insts):
		self.x = x
		self.y = y
		self.name = name
		self.data = data
		self.site_insts = site_insts

	def wires():
		return (Wire(self, i) for i in range(len(self.data.wires)))
	def pips():
		return (Pip(self, i) for i in range(len(self.data.pips)))
	def sites():
		return site_insts

class Device:
	def __init__(self, name):
		self.name = name
		self.tiles = []
		self.tiles_by_name = {}
		self.sites_by_name = {}
	def tile(name):
		return self.tiles_by_name[name]
	def site(name):
		return self.sites_by_name[name]

def import_device(name, prjxray_root, metadata_root):
	site_type_cache = {}
	tile_type_cache = {}
	def parse_xy(xy):
		xpos = xy.rfind("X")
		ypos = xy.rfind("Y")
		return int(xy[xpos+1:ypos]), int(xy[ypos+1:])

	def get_site_type_data(sitetype):
		if sitetype not in site_type_cache:
			site_type_cache[sitetype] = SiteData(sitetype)
		return site_type_cache[sitetype]
			
	def get_tile_type_data(tiletype):
		if tiletype not in tile_type_cache:
			tile_type_cache[tiletype] = TileData(tiletype)
		return tile_type_cache[tiletype]

	d = Device(name)
	with open(prjxray_root + "/gridinfo/grid-" + name + "-db.txt") as gf:
		tileprops, tilesites, siteprops = parse_gridinfo(gf)
		for tile, props in sorted(tileprops.items()):
			x = int(props["COLUMN"])
			y = int(props["ROW"])
			tiletype = props["TYPE"]
			siteinsts = []
			if tile in tilesites:
				for site in tilesites[tile]:
					sitetype = siteprops[site]["SITE_TYPE"]
					si = Site(tile, site, parse_xy(site), get_site_type_data(sitetype))
					siteinsts.append(si)
					d.sites_by_name[site] = si
			t = Tile(x, y, tile, get_tile_type_data(tiletype), siteinsts)
			d.tiles_by_name[tile] = t
			d.tiles.append(t)
	return d