#!/bin/bash

PATH=../build/bat/Build/Products/Debug/:$PATH

function report {
	if [[ $? ]]; then
		echo -e "\033[33m$TEST \033[32m OK \033[37m"
	else
		echo -e "\033[33m$TEST \033[31m FAILi \033[37m"
	fi
}

TEST="RECORD SEQ"
bat -v -S cat -C "./slowsec.sh" -r 09.txt -m 1024
report

TEST="REPLAY SEQ"
bat -S cat -s 09.txt -R
report

TEST="RUN SH"
bat -v -S sh -s 2x2.txt
report

TEST="TEST x2 SCRIPT"
bat -v -S ./2x2.sh -C ./slowsec.sh -s seqx2.txt
report

TEST="SUPERVISE x2 SCRIPT (3 FAILS)"
bat -v -S ./2x2.sh -C ./slowsec.sh -s seqx2wrong3.txt
test $? == 3
report

TEST="REPLAY x2 SCRIPT (3 FAILS)"
bat -v -S ./2x2.sh -s seqx2wrong3.txt
test $? == 3
report
