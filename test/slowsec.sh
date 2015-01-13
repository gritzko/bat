#!/bin/bash

I=1

while (( $I < 10 )); do
	echo $I
	((I=$I+1))
	sleep 0.2
done
