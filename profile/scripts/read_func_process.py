"""
IN: perf report of L2, L3 cache misses
OUT: 1)fn_list: list of functions that are responsible for most cache misses
     2)fn_percent_list: list of percentages of cache misses for each function
"""
import sys
import re

funcNum = 0
percentSum = 0
#funcList = []
#percentList = []
fn_to_pct_map = {}

bench = sys.argv[3]

MAX_FUNC_NUM = 20
with open(sys.argv[1]) as file_in:
    lines = []
    for line in file_in:
        # check if it is substr
        print(line.split('$')[2], bench)
        if line.split('$')[2] in bench or bench in line.split('$')[2]: # command = bench
            fn_name_prim = line.split('$')[3]
            fn_name = fn_name_prim.split()[1]
            for i in range(2, len(fn_name_prim.split())):
                fn_name += " "
                fn_name += fn_name_prim.split()[i]

            overhead_pct = float(line.split("%")[0])
            if overhead_pct < 1:
                continue

            if fn_name in fn_to_pct_map:
                fn_to_pct_map[fn_name] += overhead_pct
            else:
                if funcNum >= MAX_FUNC_NUM:
                    continue
                funcNum=funcNum+1
                fn_to_pct_map[fn_name] = overhead_pct


#output_file_func_percent_list = open(sys.argv[2], "a")
output_file_func_list = open(sys.argv[2], "a")


if fn_to_pct_map:
    print("Functions that cause most cache misses (L2, L3 aggregated, total 200%):")
    for fn, pct in fn_to_pct_map.items():
        print(f"{fn} {str(pct)}%");
        output_file_func_list.write(f"{fn}\t{str(pct)}%\n")
