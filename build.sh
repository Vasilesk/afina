#!/bin/bash
rm -r -f "build"
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Debug ..
make
