#!/bin/bash

cd example && ./run.sh &
python tests/test.py
exit $?
