#!/bin/bash
SAMPLING_RATE_IN_CNTS=$1
TARGET=$2
shift 2
ARGS="$*"

TARGET_ABS_PATH=$(readlink -f $TARGET)
TARGET_BASENAME=$(basename $TARGET_ABS_PATH)

cd "$(dirname "$0")"

MAIN_DIR=../..
BUILD_DIR=${MAIN_DIR}/build
CRLIST=${MAIN_DIR}/crlist.txt
RES_DIR=../results
TMP_DIR=../results/tmp
SCHED_NAME=sched.out
#TARGET=$(cat ${CRLIST} | awk '{print $1}')

echo "Capture deliquent load PCs ... "
echo "Target object: "
#echo -n "$TARGET_ABS_PATH " > $CRLIST
#echo $ARGS >> $CRLIST
#cat $CRLIST

mkdir -p ${RES_DIR}

rm -r ${TMP_DIR} 2> /dev/null
mkdir -p ${TMP_DIR}

echo "  1) perf record L2/L3 misses"
# TODO: determine sampling freq, type of events
#perf record -e cpu/event=0xd1,umask=0x20,name=MEM_LOAD_RETIRED.L3_MISS/ppp -- $benchmark_path/$benchmark_name 1 1 $input_graphs_path/$g
EVENT1=cpu/event=0xd1,umask=0x10,name=MEM_LOAD_RETIRED.L2_MISS/ppp
EVENT2=cpu/event=0xd1,umask=0x20,name=MEM_LOAD_RETIRED.L3_MISS/ppp
EVENT3=BR_INST_RETIRED.ALL_BRANCHES
sudo perf record -e ${EVENT1},${EVENT2},${EVENT3}\
        -b \
        -c${SAMPLING_RATE_IN_CNTS} \
        -o ${RES_DIR}/perf.data -- sudo sh -c ". ${MAIN_DIR}/config/set_env.sh; chrt -r 99 time -p ${TARGET_ABS_PATH} ${ARGS}"

sudo chown ${USER} ${RES_DIR}/perf.data

#perf record -e cpu/event=0xd1,umask=0x10,name=MEM_LOAD_RETIRED.L2_MISS/ppp,cpu/event=0xd1,umask=0x20,name=MEM_LOAD_RETIRED.L3_MISS/ppp,cycles:u -b -F1000 -o ${RES_DIR}/perf.data -- ${BUILD_DIR}/${SCHED_NAME} ${CRLIST}
#perf record -e cpu/event=0xd1,umask=0x20,name=MEM_LOAD_RETIRED.L3_MISS/ppp -F1000 -o ${RES_DIR}/perf.data -- ${BUILD_DIR}/${SCHED_NAME} ${CRLIST}

echo "  2) perf report"
perf report -i ${RES_DIR}/perf.data --sort comm,dso,symbol -t"\$" |
    sed '/BR_INST_RETIRED.ALL_BRANCHES/q' | grep "%" > ${TMP_DIR}/perf_report.txt


echo "  3) Capture Functions that cause most L2/L3 misses"
python3 read_func.py ${TMP_DIR}/perf_report.txt ${TMP_DIR}/fn_list.txt ${TARGET_BASENAME} #${SCHED_NAME}

echo "  4) perf annotate --stdio  &  Capturing all deliquent load PCs for each function ...."
echo -n > ${RES_DIR}/all_PClist.txt

IFS=$'\n'
for FUNC in $(cat ${TMP_DIR}/fn_list.txt | awk -F'[\t]' '{print $1}'); do
  echo "    Function Name: $FUNC"
  echo "    L2 misses "
  FUNC_NO_SPACE=$(echo $FUNC | sed 's/ /_/g')

  perf annotate -i ${RES_DIR}/perf.data --stdio -M intel "${FUNC}" |
    sed -n '/MEM_LOAD_RETIRED.L2_MISS/,/\(MEM_LOAD_RETIRED.L3_MISS\|BR_INST_RETIRED\)/{p;/\(MEM_LOAD_RETIRED.L3_MISS\|BR_INST_RETIRED\)/q}' |
    sed '1d' | sed '$d' > ${TMP_DIR}/fn_${FUNC_NO_SPACE}_l2_annotate.txt

  echo "    L3 misses "
  perf annotate -i ${RES_DIR}/perf.data --stdio -M intel "${FUNC}" |
    sed -n '/MEM_LOAD_RETIRED.L3_MISS/,/BR_INST_RETIRED/{p;/BR_INST_RETIRED/q}' |
    sed '1d' | sed '$d' > ${TMP_DIR}/fn_${FUNC_NO_SPACE}_l3_annotate.txt


  python3 llc_missed_pcs_rfile.py ${TMP_DIR}/fn_${FUNC_NO_SPACE}_l2_annotate.txt ${TMP_DIR}/fn_${FUNC_NO_SPACE}_percent.txt ${TMP_DIR}/fn_${FUNC_NO_SPACE}_PClist.txt
  python3 llc_missed_pcs_rfile.py ${TMP_DIR}/fn_${FUNC_NO_SPACE}_l3_annotate.txt ${TMP_DIR}/fn_${FUNC_NO_SPACE}_percent.txt ${TMP_DIR}/fn_${FUNC_NO_SPACE}_PClist.txt

  unset IFS
  for PC in $(cat ${TMP_DIR}/fn_${FUNC_NO_SPACE}_PClist.txt); do
    echo -n "${TARGET_ABS_PATH} " >> ${RES_DIR}/all_PClist.txt
    echo ${PC} >> ${RES_DIR}/all_PClist.txt
  done
  IFS=$'\n'

  sort -u ${RES_DIR}/all_PClist.txt -o ${RES_DIR}/all_PClist.txt
done
unset IFS

#rm -r ${TMP_DIR} 2> /dev/null
echo "Instruction capture done!"
