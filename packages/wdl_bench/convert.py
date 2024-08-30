import json
import sys

import parse_line

sum_c = {}

input_file_name = "out_" + sys.argv[1] + ".txt"


with open(input_file_name) as f:
    if sys.argv[1] == "concurrent_hash_map_benchmark":
        parse_line.parse_line_chm(f, sum_c)
    elif sys.argv[1] == "lzbench":
        parse_line.parse_line_lzbench(f, sum_c)
    elif sys.argv[1] == "openssl":
        parse_line.parse_line_openssl(f, sum_c)
    else:
        parse_line.parse_line(f, sum_c)

out_file_name = "out_" + sys.argv[1] + ".json"
with open(out_file_name, "w") as f:
    json.dump(sum_c, f, indent=4, sort_keys=True)
