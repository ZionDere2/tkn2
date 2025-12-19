#!/bin/bash

set -e

if [[ ! $# -ge 1 ]]; then
    echo "Missing argument."
    echo "Usage: '$0 praxis{0,1,2,3}'"
    echo "Optional: --reinstall. If set, python modules will be reinstalled."
    exit 1
fi

if [[ $1 = "praxisX" ]]; then
    echo "Please specify task number."
    echo "Usage: '$0 praxis{0,1,2,3}'"
    echo "Optional: --reinstall. If set, python modules will be reinstalled."
    exit 1
fi

# python module install
venv_path="$(pwd)/.venv"
lock_path="$(pwd)/.rnvs_lock"

pytest_args="${@:2}"
if [ "$2" = "--reinstall" ]; then
    if [ -f "$lock_path" ]; then
	    rm "$lock_path"
    fi
    if [ -d "$venv_path" ]; then
	    rm -r "$venv_path"
    fi
    pytest_args="${@:3}"
fi

if [ ! -f "$lock_path" ]; then
	echo "creating virtualenv at $venv_path..."
	if ! python3 -m venv "$venv_path" &> /dev/null; then
		echo "venv module is not available. installing requirements with pip..."
		python3 -m pip install --ignore-installed -r requirements.txt
		rm -r "$venv_path"
	else
		source "$venv_path/bin/activate"
		echo "installing requirements..."
		python3 -m pip install -r requirements.txt
	fi
	touch "$lock_path"
fi

if [ -d "$venv_path" ] && [ "$VIRTUAL_ENV" != "$venv_path" ]; then
    source "$venv_path/bin/activate" &> /dev/null
fi
# done python

TEST_SCRIPT="$(dirname $0)/test_$1.py"

if [[ ! -e ${TEST_SCRIPT} ]]; then
    echo "Requested test file ${TEST_SCRIPT} does not exist!"
    exit 1
fi

echo "Packaging submission..."
PACKAGING_DIR=$(mktemp -d)
cmake -B${PACKAGING_DIR}
cmake --build ${PACKAGING_DIR} -t package_source

# copy tar to current folder for submitting
cp -f $(find ${PACKAGING_DIR} -name '*.tar.gz' | head -n1)  "$(pwd)"

echo "Extracting submission..."
SOURCE_DIR=$(mktemp -d)
(cd ${SOURCE_DIR} && find ${PACKAGING_DIR} -name '*.tar.gz' | head -n1 | xargs tar -xzf)
EXTRACTED_SOURCE_DIR=$(find ${SOURCE_DIR} -maxdepth 1 -mindepth 1 -type d | head -n1)

echo "Building submission..."
BUILD_DIR=$(mktemp -d)
cmake -S"${EXTRACTED_SOURCE_DIR}" -B${BUILD_DIR}
TARGET="webserver"
EXECUTABLE="--executable=${BUILD_DIR}/webserver"
if [[ "$1" == "praxis0" ]]; then
	TARGET="hello_world"
	EXECUTABLE="--executable=${BUILD_DIR}/hello_world"
elif [[ "$1" == "praxis3" ]]; then
	TARGET="zmq_distributor zmq_worker"
	EXECUTABLE="--dist_exec=${BUILD_DIR}/zmq_distributor --work_exec=${BUILD_DIR}/zmq_worker"
fi
cmake --build ${BUILD_DIR} -t ${TARGET}

echo "Executing tests..."
python3 -m pytest -o cache_dir=${BUILD_DIR} ${TEST_SCRIPT} ${EXECUTABLE} ${pytest_args}
