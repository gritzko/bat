#!/bin/bash

while read num; do
	res=$(($num*$num))
	date=`date +%H:%M:%S`
	echo $date $num*$num=$res
done

