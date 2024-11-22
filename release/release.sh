#!/bin/bash
# To customize the version name:
#     VERSION=myversion ./release.sh
set -e

cd "$(dirname ${BASH_SOURCE[0]})"
rm -rf output

./test_server.sh
./test_client.sh

./build_server.sh
./build_windows.sh 32
./build_windows.sh 64

./package_server.sh
./package_client.sh win32
./package_client.sh win64

./generate_checksums.sh

echo "Release generated in $PWD/output"