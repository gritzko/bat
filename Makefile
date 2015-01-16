PATHS="-L/usr/local/lib -I/usr/local/include"

all:: prepare build test todo


test::
	cd test && ./RUN.sh

build:: bat/bat.c
	gcc -o build/bat bat/bat.c $(PATHS) -lpcre2-8

prepare:
	if [ ! -e build/ ]; then mkdir build; fi

clean:
	rm -rf build/

todo::
	@echo === GROUND LEVEL BUG/ISSUE TRACKER ===
	@git grep -w --color -n -P 'TO\DO|FIX\ME'
	@echo

