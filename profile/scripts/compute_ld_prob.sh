#!/bin/bash
if [ $# -ne 3 ]; then
    echo "Usage: $0 <perf.data> <is-scav> <SAMPLING_RATE_IN_CNTS>"
    exit 1
fi

PERF_DATA_PATH=$1
IS_SCAV=$2
PERF_ABS_PATH=$(readlink -f $PERF_DATA_PATH)
SAMPLING_RATE_IN_CNTS=$3
#SAMPLING_RATE_IN_CNTS=$(perf script -i ${PERF_ABS_PATH} --header | grep BR_INST | grep "sample_freq" | awk -F'[, ]' '{print $256}')
echo ${SAMPLING_RATE_IN_CNTS}
cd "$(dirname "$0")"

MAIN_DIR=../../
BUILD_DIR=${MAIN_DIR}/build
RES_DIR=../results
TMP_DIR=../results/tmp

mkdir -p ${TMP_DIR}

# symbol map
perf report -i ${PERF_ABS_PATH} --dump-raw-trace | grep PERF_RECORD_MMAP2 | \
    awk -F'[][() ]' '{print $NF, $9, $10, $13}' > ${TMP_DIR}/symbol.map  # <objname> <start-address> <length> <pgoffset>

# LBR samples
perf script -i ${PERF_ABS_PATH} -F event,brstack | grep BRANCH | \
    awk -F':' '{print $2}' > ${TMP_DIR}/lbr.samples

# L2, L3 summary
perf script -i ${PERF_ABS_PATH} -F event,ip | grep "MEM_LOAD_RETIRED.L2_MISS" | \
    awk '{cnt[$2]+=1} END {for (ip in cnt) print ip, cnt[ip]}' > ${TMP_DIR}/l2.summary

perf script -i ${PERF_ABS_PATH} -F event,ip | grep "MEM_LOAD_RETIRED.L3_MISS" | \
    awk '{cnt[$2]+=1} END {for (ip in cnt) print ip, cnt[ip]}' > ${TMP_DIR}/l3.summary

# finally, compute prob
python3 compute_ld_prob.py ${TMP_DIR}/lbr.samples \
    ${TMP_DIR}/symbol.map \
    ${RES_DIR}/all_PClist.txt \
    ${TMP_DIR}/l2.summary \
    ${TMP_DIR}/l3.summary \
    ${SAMPLING_RATE_IN_CNTS} \
    $IS_SCAV > ${RES_DIR}/ld_prob.txt

cat ${RES_DIR}/ld_prob.txt | grep "LOAD_PROB" | awk -F'[% ]' '{print $2, $3, $7, $9}' > ${RES_DIR}/cmpc_list.txt
if [ $IS_SCAV -eq 1 ]; then
    cat ${RES_DIR}/ld_prob.txt | grep "LAT_PROF" > ${RES_DIR}/lat_prof.txt
    cat ${RES_DIR}/ld_prob.txt | grep "PRED_PROF" > ${RES_DIR}/pred_prof.txt
fi

#rm -r ${TMP_DIR} 2> /dev/null
