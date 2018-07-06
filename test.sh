#!/bin/bash

cd example && ./run.sh &

until $(curl --output /dev/null -k --silent --fail http://localhost:9090/); do
    echo 'Waiting for uwsgi...'
    sleep 1
done

python tests/test.py
exit $?
