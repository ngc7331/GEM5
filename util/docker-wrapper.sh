#!/bin/bash

CMD=$*
IMAGE=ngc7331/gem5-ubuntu-22.04_all-dependencies-xs

PWD=$(pwd)
PARENT_PWD=$(dirname ${PWD})

# sanity check
if [ ! -d ${PARENT_PWD}/.git ]; then
  echo "Error: ${PARENT_PWD} is not a git repository"
  exit 1
fi

docker run -it --rm --user ${UID}:${GID} --volume ${PARENT_PWD}:${PARENT_PWD}:rw ${IMAGE} \
  bash -c "cd ${PWD} && ${CMD}"
