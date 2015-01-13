#!/bin/bash

PATH=../build/bat/Build/Products/Debug/:$PATH

bat -S cat -C "./slowsec.sh" -r 09.txt -m 1024
bat -S cat -s 09.txt -R

