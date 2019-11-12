constids = []
str_to_id = {}
num_base_ids = 0

def make(s):
	global constids, str_to_id
	if s not in str_to_id:
		str_to_id[s] = len(constids)
		constids.append(s)
	return str_to_id[s]

def read_base(f):
	global num_base_ids, constids
	assert len(constids) == 0
	make("") # constid 0 is always the empty string
	num_base_ids += 1

	for line in f:
		sl = line.strip()
		if len(sl) < 3 or sl[0:2] != "X(":
			continue
		make(sl[2:-1])
		num_base_ids += 1

	return num_base_ids