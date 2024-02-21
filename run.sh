#!/bin/bash

GEM5_HOME=$(pwd)
GEM5=$GEM5_HOME/build/RISCV/gem5.opt
CONFIG=$GEM5_HOME/configs/example/fs.py

BIN=${BIN:-coremark-1000}
BIN_FILE=${GEM5_HOME}/../NutShell/ready-to-run/${BIN}.bin

RUN_CMD="$GEM5 $CONFIG \
    --xiangshan-system --cpu-type=DerivO3CPU \
    --mem-type=DRAMsim3 \
    --dramsim3-ini=${GEM5_HOME}/ext/dramsim3/xiangshan_configs/xiangshan_DDR4_8Gb_x8_3200_2ch.ini \
    --mem-size=8GB \
    --caches --cacheline_size=64 \
    --l1i_size=64kB --l1i_assoc=8 \
    --l1d_size=64kB --l1d_assoc=8 \
    --l1d-hwp-type=XSCompositePrefetcher \
    --short-stride-thres=0 \
    --l2cache --l2_size=1MB --l2_assoc=8 \
    --l3cache --l3_size=16MB --l3_assoc=16 \
    --l1-to-l2-pf-hint \
    --l2-hwp-type=WorkerPrefetcher \
    --l2-to-l3-pf-hint \
    --l3-hwp-type=WorkerPrefetcher \
    --bp-type=DecoupledBPUWithFTB --enable-loop-predictor \
    --generic-rv-cpt=${BIN_FILE} --raw-cpt \
"

./util/docker-wrapper.sh "${RUN_CMD}"
