export LD_PRELOAD=""

PROF_DIR=profile
SAMPLING_RATE_IN_CNT=102400
BIN=$1
shift 1
ARGS="$*"
echo $ARGS

${PROF_DIR}/scripts/capture_cm_inst_primary.sh ${SAMPLING_RATE_IN_CNT} ${BIN} ${ARGS}
${PROF_DIR}/scripts/compute_ld_prob.sh profile/results/perf.data 0 $SAMPLING_RATE_IN_CNT
