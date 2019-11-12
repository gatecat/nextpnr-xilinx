# Return a dtuple:
#  - dictionary of {tile name -> dict of tile properties} 
#  - dictionary of (tile name -> list of sites)
#  - dictionary of (site name -> dict site properties)
def parse_gridinfo(f):
	tiles = {}
	tilesites = {}
	sites = {}
	for line in f:
		sl = line.strip().split(" ", 3)
		if len(sl) < 2:
			continue
		if sl[0] == "TILEPROP":
			if sl[1] not in tiles:
				tiles[sl[1]] = {}
			tiles[sl[1]][sl[2]] = sl[3]
		elif sl[0] == "SITEPROP":
			if sl[1] not in sites:
				sites[sl[1]] = {}
			sites[sl[1]][sl[2]] = sl[3] if len(sl) > 3 else ""
		elif sl[0] == "TILESITE":
			if sl[1] not in tilesites:
				tilesites[sl[1]] = []
			tilesites[sl[1]].append(sl[2])
	return (tiles, tilesites, sites)
