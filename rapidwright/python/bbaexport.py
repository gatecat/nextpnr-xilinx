from xilinx_device import *
from bba import BBAWriter
import sys, argparse
import bels, constid

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--xray", help="Project X-Ray device database path for current family (e.g. ../prjxray-db/artix7)", type=str, required=True)
	parser.add_argument("--metadata", help="nextpnr-xilinx site metadata root", type=str, required=True)
	parser.add_argument("--device", help="name of device to export", type=str, required=True)
	parser.add_argument("--constids", help="name of nextpnr constids file to read", type=str, required=True)
	parser.add_argument("--bba", help="bba file to write", type=str, required=True)
	args = parser.parse_args()
	with open(args.constids, "r") as cf:
		constid.read_base(cf)
if __name__ == '__main__':
	main()