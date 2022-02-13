#!/bin/bash
for (( size=100; size<=3100; size+=100 ))
do
	make clean && make VEC_PER_CLASS=100 CLASS_NUM=$size && ./predict 1
done
