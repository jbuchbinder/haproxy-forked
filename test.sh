#!/bin/bash

make clean && make TARGET=linux26 USE_API=1 && ./haproxy  -db -f examples/haproxy.api.cfg

