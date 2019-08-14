# Obtain the set of invertible pins from Yosys sim & blackbox models
import re
import sys

module_re = re.compile(r'\s*module ([A-Z0-9_a-z]+).*')
invparam_re = re.compile(r'\s*parameter\s+\[(\d+):0\]\s+IS_([A-Za-z0-9_]+)_INVERTED.*')
invparam_2_re = re.compile(r'\s*parameter\s+IS_([A-Za-z0-9_]+)_INVERTED.*')

for filename in sys.argv[1:]:
    with open(filename, 'r') as f:
        modname = None
        for line in f:
            mm = module_re.match(line)
            if mm:
                modname = mm.group(1)
                continue
            im = invparam_re.match(line)
            if im:
                pname = im.group(2)
                ubound = int(im.group(1))
                if modname[0:2] == "FD" and pname == "D":
                    continue # IS_D_INVERTED isn't heavily used
                if ubound == 0:
                    params = [pname]
                else:
                    params = ["%s[%d]" % (pname, i) for i in range(ubound + 1)]
                for p in params:
                    print('        invertible_pins[ctx->id("%s")].insert(ctx->id("%s"));' % (modname, p))

            im2 = invparam_2_re.match(line)
            if im2:
                pname = im2.group(1)
                if modname[0:2] == "FD" and pname == "D":
                    continue # IS_D_INVERTED isn't heavily used
                params = [pname]
                for p in params:
                    print('        invertible_pins[ctx->id("%s")].insert(ctx->id("%s"));' % (modname, p))