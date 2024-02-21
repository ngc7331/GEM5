#!/bin/bash

BUILD_CMD="scons build/RISCV/gem5.opt --gold-linker -j8"

./util/docker-wrapper.sh "${BUILD_CMD}"
