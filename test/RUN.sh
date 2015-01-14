#!/bin/bash

PATH=../build/bat/Build/Products/Debug/:$PATH

bat -v -S cat -C "./slowsec.sh" -r 09.txt -m 1024
echo RECORD SEQ $?

bat -S cat -s 09.txt -R
echo REPLAY SEQ $?

bat -v -S sh -s 2x2.txt
echo RUN SH $?

bat -v -S ./2x2.sh -C ./slowsec.sh -s seqx2.txt
echo TEST x2 SCRIPT $?

bat -v -S ./2x2.sh -C ./slowsec.sh -s seqx2wrong3.txt
echo TEST x2 SCRIPT 3=$?
