#!/bin/bash

cd ext/dramsim3
if [ ! -d DRAMsim3 ]; then
    git clone https://github.com/umd-memsys/DRAMsim3.git DRAMsim3
fi
mkdir -p DRAMsim3/build
cd -

BUILD_CMD="cd ext/dramsim3/DRAMsim3/build && cmake .. && make"

./util/docker-wrapper.sh "${BUILD_CMD}"
