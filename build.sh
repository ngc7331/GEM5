#!/bin/bash

USE_DOCKER=${USE_DOCKER:-true}

BUILD_CMD="scons build/RISCV/gem5.opt --gold-linker -j8"

if [ "$USE_DOCKER" == "true" ]; then
    ./util/docker-wrapper.sh "${BUILD_CMD}"
else
    eval "${BUILD_CMD}"
fi
