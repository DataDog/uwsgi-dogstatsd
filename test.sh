#!/bin/bash

cd example && ./run.sh &
python tests/test.py
killall -9 uwsgi
