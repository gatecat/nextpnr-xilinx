import json

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

class TileData:
	def __init__(self, tile_type):
		self.tile_type = tile_type
		self.sites = []
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
	def __init__(self, tile, name, grid_xy, index):
		self.tile = tile 
		self.name = name
		self.grid_xy = grid_xy
		self.index = index
		self.data = tile.get_site_data(index)

	def bels(self):
		return (SiteBEL(self, i) for i in range(len(self.data.bels)))


class Tile:
	def __init__(self, x, y, name, data):
		self.x = x
		self.y = y
		self.name = name
		self.data = data

	def wires():
		return (Wire(self, i) for i in range(len(self.data.wires)))
	def pips():
		return (Pip(self, i) for i in range(len(self.data.pips)))
	def sites():
		return (Site(self, i) for i in range(len(self.data.sites)))
