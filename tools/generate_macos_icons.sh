#!/bin/sh

set -eu

if [ "$#" -ne 2 ]; then
    printf 'usage: %s <512px-source.png> <output-directory>\n' "$0" >&2
    exit 2
fi

source_path=$1
output_directory=$2
mkdir -p "$output_directory"
sips -s format icns "$source_path" --out "$output_directory/ccs-trans.icns" >/dev/null
sips -z 36 36 "$source_path" --out "$output_directory/ccs-trans-statusTemplate.png" >/dev/null
