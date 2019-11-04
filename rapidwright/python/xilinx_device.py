import json
import os
from gridinfo import parse_gridinfo
from tileconn import apply_tileconn
# Represents Xilinx device data from PrjXray etc

class WireData:
	def __init__(self, index, name, tied_value=None):
		self.index = index
		self.name = name
		self.tied_value = tied_value

class PIPData:
	def __init__(self, index, from_wire, to_wire, is_bidi, is_route_thru):
		self.index = index
		self.from_wire = from_wire
		self.to_wire = to_wire
		self.is_bidi = is_bidi
		self.is_route_thru = is_route_thru

class SiteWireData:
	def __init__(self, name, is_pin=False):
		self.name = name
		self.is_pin = is_pin

class SiteBELPinData:
	def __init__(self, name, pindir, site_wire_idx):
		self.name = name
		self.pindir = pindir
		self.site_wire_idx = site_wire_idx

class SiteBELData:
	def __init__(self, name, bel_type, bel_class, pins):
		self.name = name
		self.bel_type = bel_type
		self.bel_class = bel_class
		self.pins = pins

class SitePIPData:
	def __init__(self, bel_idx, bel_input, from_wire_idx, to_wire_idx):
		self.bel_idx = bel_idx
		self.bel_input = bel_input
		self.from_wire_idx = from_wire_idx
		self.to_wire_idx  = to_wire_idx

class SitePinData:
	def __init__(self, name, pindir, site_wire_idx, prim_pin_name):
		self.name = name
		self.dir = pindir
		self.site_wire_idx = site_wire_idx
		self.prim_pin_name  = prim_pin_name

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
		self.wires_by_name = {}
		self.pips = []
		self.sitepin_to_wire = {} # (prefix, relxy, pin) -> wireidx

class PIP:
	def __init__(self, tile, index):
		self.tile = tile
		self.index = index
		self.data = tile.get_pip_data(index)
	def src_wire(self):
		return Wire(self.tile, self.data.from_wire)
	def dst_wire(self):
		return Wire(self.tile, self.data.to_wire)
	def is_route_thru(self):
		return self.data.is_route_thru

class Wire:
	def __init__(self, tile, index):
		self.tile = tile
		self.index = index
		self.data = tile.get_wire_data(index)
	def name(self):
		return self.data.name
	def node(self):
		if self.index not in self.tile.wire_to_node:
			self.tile.wire_to_node[self.index] = Node([self])
		return self.tile.wire_to_node[self.index]

class SiteWire:
	def __init__(self, site, index):
		self.site = site
		self.index = index
		self.data = self.site.get_wire_data(index)
	def name(self):
		return self.data.name

class SiteBELPin:
	def __init__(self, bel, name):
		self.bel = bel
		self.name = name
		self.data = self.bel.data.pins[name]

class SiteBEL:
	def __init__(self, site, index):
		self.site = site
		self.index = index
		self.data = site.get_bel_data(index)
	def name(self):
		return self.data.name
	def bel_type(self):
		return self.data.bel_type
	def bel_class(self):
		return self.data.bel_class
	def pins(self):
		return (SiteBELPin(self, n) for n in self.data.pins.keys())

class SitePIP:
	def __init__(self, site, index):
		self.site = site
		self.index = index
		self.data = site.get_pip_data(index)
	def bel(self):
		return SiteBEL(self.site, self.data.bel_idx)
	def bel_input(self):
		return self.data.bel_input
	def src_wire(self):
		return SiteWire(self.site, self.data.from_wire_idx)
	def dst_wire(Self):
		return SiteWire(self.site, self.data.to_wire_idx)

class SitePin:
	def __init__(self, site, index):
		self.site = site
		self.index = index
		self.data = site.data.pins[index]
	def name(self):
		return self.data.name
	def dir(self):
		return self.data.dir
	def site_wire(self):
		return SiteWire(self.site, self.data.site_wire_idx)
	def tile_wire(self):
		return self.site.tile.site_pin_wire(self.site.prefix, self.site.rel_xy(), self.data.prim_pin_name)

class Site:
	def __init__(self, tile, name, grid_xy, data, primary=None):
		self.tile = tile
		self.name = name
		self.prefix = name[0:name.rfind('_')]
		self.grid_xy = grid_xy
		self.data = data
		self.primary = primary
		self._rel_xy = None # filled later
	def get_bel_data(self, index):
		return self.data.bels[index]
	def get_wire_data(self, index):
		return self.data.wires[index]
	def get_pip_data(self, index):
		return self.data.pips[index]
	def site_type(self):
		return self.data.site_type
	def rel_xy(self):
		if self._rel_xy is None:
			base_x = 999999
			base_y = 999999
			for site in self.tile.sites():
				if site.prefix != self.prefix:
					continue
				base_x = min(base_x, site.grid_xy[0])
				base_y = min(base_y, site.grid_xy[1])
			self._rel_xy = (self.grid_xy[0] - base_x, self.grid_xy[1] - base_y)
		return self._rel_xy
	def bels(self):
		return (SiteBEL(self, i) for i in range(len(self.data.bels)))
	def wires(self):
		return (SiteWire(self, i) for i in range(len(self.data.wires)))
	def pips(self):
		return (SitePIP(self, i) for i in range(len(self.data.pips)))
	def pins(self):
		return (SitePin(self, i) for i in range(len(self.data.pins)))
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
		self.wire_to_node = {}

	def get_pip_data(self, i):
		return self.data.pips[i]
	def get_wire_data(self, i):
		return self.data.wires[i]
	def tile_type(self):
		return self.data.tile_type
	def wires(self):
		return (Wire(self, i) for i in range(len(self.data.wires)))
	def wire(self, name):
		return Wire(self, self.data.wires_by_name[name].index)
	def pips(self):
		return (PIP(self, i) for i in range(len(self.data.pips)))
	def sites(self):
		return self.site_insts
	def site_pin_wire(self, prefix, rel_xy, pin):
		wire_idx = self.data.sitepin_to_wire[(prefix, rel_xy, pin)]
		return Wire(self, wire_idx) if wire_idx is not None else None

class Node:
	def __init__(self, wires=[]):
		self.wires = wires

class Device:
	def __init__(self, name):
		self.name = name
		self.tiles = []
		self.tiles_by_name = {}
		self.tiles_by_xy = {}
		self.sites_by_name = {}
	def tile(self, name):
		return self.tiles_by_name[name]
	def site(self, name):
		return self.sites_by_name[name]

def import_device(name, prjxray_root, metadata_root):
	site_type_cache = {}
	tile_type_cache = {}
	tile_json_cache = {}
	def parse_xy(xy):
		xpos = xy.rfind("X")
		ypos = xy.rfind("Y")
		return int(xy[xpos+1:ypos]), int(xy[ypos+1:])

	def get_site_type_data(sitetype):
		if sitetype not in site_type_cache:
			sd = SiteData(sitetype)
			sp = metadata_root + "/site_type_" + sitetype + ".json"
			if os.path.exists(sp):
				with open(sp, "r") as jf:
					sj = json.load(jf)
				for vtype, vdata in sorted(sj.items()): # Consider all site variants
					if vtype == sitetype:
						vd = sd # primary variant
					else:
						vd = SiteData(vtype)
					site_wire_by_name = {}
					def wire_index(name):
						if name not in site_wire_by_name:
							idx = len(vd.wires)
							vd.wires.append(SiteWireData(name=name))
							site_wire_by_name[name] = idx
						return site_wire_by_name[name]
					# Import bels
					bel_idx_by_name = {}
					for bel, beldata in sorted(vdata["bels"].items()):
						belpins = {}
						for pin, pindata in sorted(beldata["pins"].items()):
							belpins[pin] = SiteBELPinData(name=pin, pindir=pindata["dir"], site_wire_idx=wire_index(pindata["wire"]))
						bd = SiteBELData(name=bel, bel_type=beldata["type"], bel_class=beldata["class"], pins=belpins)
						bel_idx_by_name[bel] = len(vd.bels)
						vd.bels.append(bd)
					# Import pips
					for pipdata in vdata["pips"]:
						bel_idx = bel_idx_by_name[pipdata["bel"]]
						bel_data = vd.bels[bel_idx]
						vd.pips.append(SitePIPData(bel_idx=bel_idx_by_name[pipdata["bel"]], bel_input=pipdata["from_pin"], 
							from_wire_idx=bel_data.pins[pipdata["from_pin"]].site_wire_idx,
							to_wire_idx=bel_data.pins[pipdata["to_pin"]].site_wire_idx))
					# Import pins
					for pin, pindata in sorted(vdata["pins"].items()):
						vd.pins.append(SitePinData(name=pin, pindir=pindata["dir"], site_wire_idx=wire_index(pindata["wire"]),
							prim_pin_name=pindata["primary"]))
					sd.variants[vtype] = vd
			site_type_cache[sitetype] = sd
		return site_type_cache[sitetype]

	def read_tile_type_json(tiletype):
		if tiletype not in tile_json_cache:
			with open(prjxray_root + "/tile_type_" + tiletype + ".json", "r") as jf:
				tile_json_cache[tiletype] = json.load(jf)
		return tile_json_cache[tiletype]

	def get_tile_type_data(tiletype):
		if tiletype not in tile_type_cache:
			td = TileData(tiletype)
			# Import wires and pips 
			tj = read_tile_type_json(tiletype)
			for wire in sorted(tj["wires"].keys()):
				wire_id = len(td.wires)
				wd = WireData(index=wire_id, name=wire, tied_value=None) # FIXME: tied_value
				td.wires.append(wd)
				td.wires_by_name[wire] = wd
			for pip, pipdata in sorted(tj["pips"].items()):
				# FIXME: pip/wire delays
				pip_id = len(td.pips)
				pd = PIPData(index=pip_id,
					from_wire=td.wires_by_name[pipdata["src_wire"]].index, to_wire=td.wires_by_name[pipdata["dst_wire"]].index,
					is_bidi=~bool(pipdata["is_directional"]), is_route_thru=bool(pipdata["is_pseudo"]))
				td.pips.append(pd)
			for sitedata in tj["sites"]:
				rel_xy = parse_xy(sitedata["name"])
				prefix = sitedata["prefix"]
				for sitepin, pindata in sorted(sitedata["site_pins"].items()):
					if pindata is None:
						pinwire = None
					else:
						pinwire = td.wires_by_name[pindata["wire"]].index
					td.sitepin_to_wire[(prefix, rel_xy, sitepin)] = pinwire
			tile_type_cache[tiletype] = td

		return tile_type_cache[tiletype]

	d = Device(name)
	with open(prjxray_root + "/gridinfo/grid-" + name + "-db.txt", "r") as gf:
		tileprops, tilesites, siteprops = parse_gridinfo(gf)
		for tile, props in sorted(tileprops.items()):
			x = int(props["COLUMN"])
			y = int(props["ROW"])
			tiletype = props["TYPE"]
			t = Tile(x, y, tile, get_tile_type_data(tiletype), [])
			if tile in tilesites:
				for site in tilesites[tile]:
					sitetype = siteprops[site]["SITE_TYPE"]
					si = Site(t, site, parse_xy(site), get_site_type_data(sitetype))
					t.site_insts.append(si)
					d.sites_by_name[site] = si
			d.tiles_by_name[tile] = t
			d.tiles_by_xy[x, y] = t
			d.tiles.append(t)
	with open(prjxray_root + "/tileconn.json", "r") as tcf:
		apply_tileconn(tcf, d)
	return d