class BBAWriter:
	def __init__(self, f):
		self.f = f
	def pre(self, s):
		print("pre {}".format(s), file=self.f)
	def post(self, s):
		print("post {}".format(s), file=self.f)
	def push(self, s):
		print("push {}".format(s), file=self.f)
	def offset32(self):
		print("offset32", file=self.f)
	def ref(self, r, comment=""):
		print("ref {} {}".format(r, comment), file=self.f)
	def str(self, s, comment=""):
		print("str |{}| {}".format(s, comment), file=self.f)
	def align(self):
		print("align", file=self.f)
	def label(self, s):
		print("label {}".format(s), file=self.f)
	def u8(self, n, comment=""):
		print("u8 {} {}".format(int(n), comment), file=self.f)
	def u16(self, n, comment=""):
		print("u16 {} {}".format(int(n), comment), file=self.f)
	def u32(self, n, comment=""):
		print("u32 {} {}".format(int(n), comment), file=self.f)
	def pop(self):
		print("pop", file=self.f)
