import json
import sys
NextPnrFile = sys.argv[1]
input_file = open(NextPnrFile, 'r')
data = json.loads(input_file.read())
input_file.close();

for cells in data['modules']['top']['cells']:
	if (data['modules']['top']['cells'][cells]['type'] == 'PAD'):
		data['modules']['top']['cells'][cells]['type'] = 'IOB_PAD'
	
	if (data['modules']['top']['cells'][cells]['type'] == 'IOB33_INBUF_EN' or data['modules']['top']['cells'][cells]['type'] == 'IOB33_OUTBUF'):
                first_inst = (data['modules']['top']['cells'][cells]['attributes']['NEXTPNR_BEL']).find('/')
                if (first_inst >= 0):
                        second_inst = (data['modules']['top']['cells'][cells]['attributes']['NEXTPNR_BEL']).find('/',1+first_inst)
                        if (second_inst >= 0):
                                data['modules']['top']['cells'][cells]['attributes']['NEXTPNR_BEL']=data['modules']['top']['cells'][cells]['attributes']['NEXTPNR_BEL'][:first_inst]+data['modules']['top']['cells'][cells]['attributes']['NEXTPNR_BEL'][second_inst:];
	
output_file = open(NextPnrFile, 'w')
output_file.write(json.dumps(data,indent = 5, sort_keys=True))
output_file.close()
