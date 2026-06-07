#!/bin/env bash

if [ $# -ne 2 ]; then
    echo "Error: Two arguments required"
    echo "Usage: writer.sh <writefile> <writestr>"
    exit 1
fi

writefile=$1
writestr=$2

writedir=$(dirname "$writefile")

if ! mkdir -p "$writedir"; then
    echo "Error: Could not create directory ${writedir}"
    exit 1
fi

if echo "$writestr" >"$writefile"; then
    exit 0
else
    echo "Error: Could not create file ${writefile}"
    exit 1
fi
