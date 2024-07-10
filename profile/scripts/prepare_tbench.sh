#!/bin/bash
cd $(dirname "$0")

TBENCH_DIR=/home/sam/clh/tailbench-clh
TBENCH_APPS="img-dnn masstree sphinx xapian moses"

BUILD_DIR=$(readlink -f ../../apps/build)

if [ -z $1 ]; then
    echo "Usage: ./prepare_tbench.sh [build|run]"
    exit
fi

MODE=$1
if [ $MODE == "build" ]; then
    for app in $TBENCH_APPS; do
        if [ $app == "moses" ]; then
            cd $TBENCH_DIR/${app}/moses-cmd
        else
            cd $TBENCH_DIR/${app}
        fi
        make clean
        if [ $app == "sphinx" ]; then
            PKG_CONFIG_PATH=${TBENCH_DIR}/${app}/sphinx-install/lib/pkgconfig make -j16
        elif [ $app == "silo" ]; then
            ./build.sh
        else
            make -j16
        fi
    done
fi

if [ $MODE == "install" ]; then
    for app in $TBENCH_APPS; do
        target=$app
        if [ $app == "masstree" ]; then
            target="mttest"
        elif [ $app == "sphinx" ]; then
            target="decoder"
        elif [ $app == "moses" ]; then
            target="moses-cmd/moses"
        elif [ $app == "silo" ]; then
            target="out-perf.masstree/benchmarks/dbtest"
        fi

        cd $TBENCH_DIR/${app}
        cp $TBENCH_DIR/${app}/${target}_integrated ${BUILD_DIR}
    done
fi
