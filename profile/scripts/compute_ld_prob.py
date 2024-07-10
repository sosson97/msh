import sys
import time
sub_cf_dict_list = []
cf_dict = {}
lat_dict = {}
pred_dict = {}
l2m_dict = {}
l3m_dict = {}
symbol_map = {}

SAMPLING_RATE_IN_CNT=1
SYS_LBR_ENTRIES=32
LBR_SUBSAMPLE_SIZE=32
is_scav_profile = 0

last_match = None
def __symbol_map_lookup(addr: int) -> tuple:
    global last_match
    if last_match is not None:
        if last_match[1][1] <= addr and addr <= last_match[1][2]:
            return last_match[0], addr - last_match[1][0]

    for key, value in symbol_map.items():
        if value[1] <= addr and addr <= value[2]: # addr is in the range of the symbol
            last_match = (key, value)
            return key, addr - value[0] # compute offset from the object local address (not symbol local address)
    return None, 0

def __localize_address(range: tuple, allow_diff_obj=False) -> tuple:
    assert(len(range) == 2)

    src_pc = range[0]
    dst_pc = range[1]

    # check address range of src, dst using symbol map
    # then do two things
    # 1) filter out if src, dst are not in the same symbol
    # 2) if src, dst are in the same symbol, then translate them to symbol-local address
    #    by subtracting the symbol start address from src, dst
    src_obj_name, src_local_addr = __symbol_map_lookup(src_pc)
    dst_obj_name, dst_local_addr = __symbol_map_lookup(dst_pc)

    if src_obj_name is None or dst_obj_name is None:
        return None
    if (src_obj_name != dst_obj_name) and (not allow_diff_obj):
        return None

    return (src_obj_name, src_local_addr, dst_local_addr)

def create_control_flow_tuples(sub_cf_dict, sub_lat_dict, branch_src_to_dst_list: list) -> None:
    src, dst = None, None
    for i, src_to_dst in enumerate(branch_src_to_dst_list):
        if i == 0: # first item
            src = src_to_dst[0]
        else:
            dst = src_to_dst[1]
            cycles = src_to_dst[2]
            triple = __localize_address((dst, src))
            if triple is None:
                src = src_to_dst[0]
                continue

            if triple in sub_cf_dict:
                sub_cf_dict[triple] += 1
                sub_lat_dict[triple] = (sub_lat_dict[triple][0] + cycles, sub_lat_dict[triple][1] + 1)
            else:
                sub_cf_dict[triple] = 1
                sub_lat_dict[triple] = (cycles, 1)

            src = src_to_dst[0]


def create_predecessor_profile(sub_pred_dict, branch_src_to_dst_list: list) -> None:
    for i, src_to_dst in enumerate(branch_src_to_dst_list):
        src = src_to_dst[0]
        dst = src_to_dst[1]
        triple = __localize_address((src, dst), allow_diff_obj=True)
        if triple is None:
            continue

        local_addr_src = (triple[0], triple[1])
        local_addr_dst = (triple[0], triple[2])

        if local_addr_dst in sub_pred_dict:
            #do something
            if local_addr_src in sub_pred_dict[local_addr_dst]:
                sub_pred_dict[local_addr_dst][local_addr_src] += 1
            else:
                sub_pred_dict[local_addr_dst][local_addr_src] = 1
        else:
            sub_pred_dict[local_addr_dst] = {}
            sub_pred_dict[local_addr_dst][local_addr_src] = 1

def aggregate_sub_dicts(sub_dicts: list) -> None:
    sub_cf_dicts = [sub_dict[0] for sub_dict in sub_dicts]
    sub_lat_dicts = [sub_dict[1] for sub_dict in sub_dicts]
    sub_pred_dicts = [sub_dict[2] for sub_dict in sub_dicts]

    for sub_cf_dict in sub_cf_dicts:
        for key, value in sub_cf_dict.items():
            if key in cf_dict:
                cf_dict[key] += value
            else:
                cf_dict[key] = value

    if is_scav_profile == 0:
        return

    for sub_lat_dict in sub_lat_dicts:
        for key, value in sub_lat_dict.items():
            if key in lat_dict:
                lat_dict[key] = (lat_dict[key][0] + value[0], lat_dict[key][1] + value[1])
            else:
                lat_dict[key] = value

    for key, value in lat_dict.items():
        lat_dict[key] = (value[0] / value[1]) # cycles / cnt

    for sub_pred_dict in sub_pred_dicts:
        for key, value in sub_pred_dict.items():
            if key in pred_dict:
                for key2, value2 in value.items():
                    if key2 in pred_dict[key]:
                        pred_dict[key][key2] += value2
                    else:
                        pred_dict[key][key2] = value2
            else:
                pred_dict[key] = value


def sub_process_lbr(lines):
    sub_cf_dict = {}
    sub_lat_dict = {}
    sub_pred_dict = {}
    for line in lines:
        tokens = line.split()
        if len(tokens) < 2:
            continue

        # Note: python3 uses 8-byte integers by default (64-bit)
        # So no need to worry about overflow in conversion
        # (branch src, branch dst, cycles elapsed since the last LRB)
        branch_src_to_dst_list = [(int(token.split("/")[0].strip(),16), \
                                int(token.split("/")[1].strip(),16), \
                                int(token.split("/")[5].strip()))
                                for token in tokens]

        create_control_flow_tuples(sub_cf_dict, sub_lat_dict, branch_src_to_dst_list[:LBR_SUBSAMPLE_SIZE])
        if is_scav_profile == 1:
            create_predecessor_profile(sub_pred_dict, branch_src_to_dst_list[:LBR_SUBSAMPLE_SIZE])
    return sub_cf_dict, sub_lat_dict, sub_pred_dict


def process_lbr(trace_filename: str) -> None:
    f = open(trace_filename, "r")
    lines = f.readlines()

    f.close()
    src_to_dst_count = {}

    NUM_CORES = 56
    # split lines into N processes
    # each process generate control_flow_tuples and aggregate later
    sublines = [[] for i in range(NUM_CORES)]
    for i in range(NUM_CORES):
        sublines[i] = lines[i::NUM_CORES]

    import multiprocessing
    results = []
    with multiprocessing.Pool(NUM_CORES) as p:
        results = p.map(sub_process_lbr, sublines)

        p.close()
        p.join()
    print([len(dic) if dic is not None else 0 for dic in results])
    aggregate_sub_dicts(results)


    #for line in lines:
    #    tokens = line.split()
    #    if len(tokens) < 2:
    #        continue
    #
    #    # Note: python3 uses 8-byte integers by default (64-bit)
    #    # So no need to worry about overflow in conversion
    #    branch_src_to_dst_list = [(int(token.split("/")[0].strip(),16), \
    #                              int(token.split("/")[1].strip(),16))
    #                              for token in tokens]
    #
    #    create_control_flow_tuples(branch_src_to_dst_list[:LBR_SUBSAMPLE_SIZE])


    print("BB execution summary:")
    for key, value in cf_dict.items():
        print(f"{key[0]} {hex(key[1])} {hex(key[2])} {value}")
    print(" ")

    print("BB latency summary:")
    for key, value in lat_dict.items():
        print(f"LAT_PROF {key[0]} {key[1]} {key[2]} {value}")
    print(" ")

    print("BB predecessor summary:")
    for key, value in pred_dict.items():
        for key2, value2 in value.items():
            print(f"PRED_PROF {key[0]} {key[1]} {key2[0]} {key2[1]} {value2}")
    print(" ")

    #with open("cf_dict.txt", "w") as f:
    #    for key, value in cf_dict.items():
    #        f.write(f"{key[0]} {key[1]} {value}\n")

def sample_to_approx_total(exec_cnt: int, scale:int = 1) -> int:
    return exec_cnt / scale * SAMPLING_RATE_IN_CNT


def create_cm_dict(summary_filename: str, level: int) -> None:
    dic = l2m_dict if level == 2 else l3m_dict

    f = open(summary_filename, "r")
    lines = f.readlines()
    for line in lines:
        # L2/L3 summary file format:
        # <pc> <sample-count> per line
        pc = int(line.split()[0].strip(), 16)
        sample_cnt = int(line.split()[1].strip())

        obj_name, addr = __symbol_map_lookup(pc)
        if obj_name is None:
            continue
        dic[(obj_name, addr)] = sample_to_approx_total(sample_cnt)

    f.close()

# Compute the probability that each load instruction causes a cache miss
# Assumption: addresses are given in hex and symbol-local address.
def compute_prob(addrlist_filename: str) -> None:
    f = open(addrlist_filename, "r")
    #out = open("exec_counts.txt", "w")
    lines = f.readlines()

    for line in lines:
        # Address list file format:
        # <shared-object-name> <symbol-local-address> per line
        obj_name = line.split()[0].strip()
        addr = int(line.split()[1].strip(), 16)
        sample_cnt = 0

        # Loop through all control flow tuples to find the ones that include the given address
        for key, value in cf_dict.items():
            if key[0] == obj_name and key[1] <= addr and addr <= key[2]:
                 sample_cnt += value
        exec_cnt = sample_to_approx_total(sample_cnt, LBR_SUBSAMPLE_SIZE)

        l2_miss_cnt = l2m_dict[(obj_name, addr)] if (obj_name, addr) in l2m_dict else 0
        l3_miss_cnt = l3m_dict[(obj_name, addr)] if (obj_name, addr) in l3m_dict else 0

        # print prob up to two decimal digits after the point
        if exec_cnt == 0:
            print(f"LOAD_PROB {obj_name} {addr} {l2_miss_cnt} {l3_miss_cnt} {exec_cnt} 0.00% 0.00%")
            continue

        print(f"LOAD_PROB {obj_name} {addr} {l2_miss_cnt} {l3_miss_cnt} {exec_cnt}" + \
              " {0:.2f}% {1:.2f}%".format(100*l2_miss_cnt/exec_cnt, 100*l3_miss_cnt/exec_cnt))
    f.close()


def run(lbr_trace_filename: str,
        l2_summary_filename: str,
        l3_summary_filename: str,
        addrlist_filename: str) -> None:

    t1 = time.time()
    process_lbr(lbr_trace_filename)
    t2 = time.time()
    create_cm_dict(l2_summary_filename, 2)
    t3 = time.time()
    create_cm_dict(l3_summary_filename, 3)
    t4 = time.time()
    compute_prob(addrlist_filename)
    t5 = time.time()

    print(f"process_lbr: {t2 - t1} seconds")
    print(f"create_cm_dictl2: {t3 - t2} seconds")
    print(f"create_cm_dictl3: {t4 - t3} seconds")
    print(f"compute_prob: {t5 - t4} seconds")


def build_symbol_map(symbolmap_filename: str) -> None:
    f = open(symbolmap_filename, "r")
    lines = f.readlines()
    for line in lines:
#Symbol map file format:
#<shared-object-name> <symbol-start-address> <length> <offset> per line
#we want to maintain (object start address, symbol start address, symbol end address)
        tokens = line.split()
        if len(tokens) < 4:
            continue
#Assuming a dumb linear search
        symbol_map[tokens[0]] = (int(tokens[1], 16) - int(tokens[3], 16), int(tokens[1], 16), int(tokens[1], 16) + int(tokens[2], 16))
    f.close()

if __name__ == "__main__":
    if len(sys.argv) < 8:
        print("usage: " + sys.argv[0] + " <lbr-trace> <symbol-map> <address-list> <l2-summary> <l3-summary> <INT: sampling-rate-in-cnt> <is-scav-profile>")
        sys.exit(1)
    lbr_trace_filename = sys.argv[1]
    symbolmap_filename = sys.argv[2]
    addrlist_filename = sys.argv[3]
    l2_summary_filename = sys.argv[4]
    l3_summary_filename = sys.argv[5]
    SAMPLING_RATE_IN_CNT = int(sys.argv[6])
    is_scav_profile = int(sys.argv[7])

    start = time.time()
    build_symbol_map(symbolmap_filename)
    end = time.time()
    print(f"Symbol map built in {end - start} seconds")

    run(lbr_trace_filename, l2_summary_filename, l3_summary_filename, addrlist_filename)
