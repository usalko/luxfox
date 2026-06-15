#!/bin/bash
CD=$PWD
cd ulama && ./build.sh
cd $CD
cd .. && ./build.sh sync
cd $CD
