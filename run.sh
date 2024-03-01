#!/bin/bash

USE_DOCKER=${USE_DOCKER:-true}

GEM5_HOME=$(pwd)
GEM5=$GEM5_HOME/build/RISCV/gem5.opt

DEBUG_FLAGS=
if [[ -n "$DEBUG_FLAGS" ]]; then
    DEBUG_ARGS="--debug-flags=${DEBUG_FLAGS}"
else
    DEBUG_ARGS=
fi
STATS_FILE=${STATS_FILE:-stats.txt}
GEM5_ARGS=" \
    ${DEBUG_ARGS} \
    --stats-file=${STATS_FILE} \
"

CONFIG=$GEM5_HOME/configs/example/fs.py

BIN=${BIN:-coremark-1000}
BIN_FILE=${GEM5_HOME}/../ready-to-run/${BIN}.bin

FTBTAGE_NUM_PREDICTORS=${FTBTAGE_NUM_PREDICTORS:-4}
FTBTAGE_TABLE_SIZES=${FTBTAGE_TABLE_SIZES:-2048,2048,2048,2048}
FTBTAGE_TTAG_BIT_SIZES=${FTBTAGE_TTAG_BIT_SIZES:-8,8,8,8}
FTBTAGE_TTAG_PC_SHIFTS=${FTBTAGE_TTAG_PC_SHIFTS:-1,1,1,1}
FTBTAGE_HIST_LENGTHS=${FTBTAGE_HIST_LENGTHS:-8,13,32,119}

TAGE_ARGS=" \
    --ftbtage-num-predictors=${FTBTAGE_NUM_PREDICTORS} \
    --ftbtage-table-sizes=${FTBTAGE_TABLE_SIZES} \
    --ftbtage-ttag-bit-sizes=${FTBTAGE_TTAG_BIT_SIZES} \
    --ftbtage-ttag-pc-shifts=${FTBTAGE_TTAG_PC_SHIFTS} \
    --ftbtage-hist-lengths=${FTBTAGE_HIST_LENGTHS} \
"

ENABLE_BP_DB=${ENABLE_BP_DB:-false}
BP_DB_FILE=${BP_DB_FILE:-bp.db}
if [[ "$ENABLE_BP_DB" == "true" ]]; then
    BP_DB_ARGS="--enable-bp-db --bp-db-file=${BP_DB_FILE}"
else
    BP_DB_ARGS=
fi

RUN_CMD="$GEM5 $GEM5_ARGS $CONFIG \
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
    ${BP_DB_ARGS} \
    ${TAGE_ARGS} \
"

if [ "$USE_DOCKER" = "true" ]; then
    ./util/docker-wrapper.sh "${RUN_CMD}"
else
    eval "${RUN_CMD}"
fi
