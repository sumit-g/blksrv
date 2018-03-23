# Makefile to build the ramdisk example

all : ramdisk

ramdisk : ramdisk.cc ../lib/libblksrv.a
	g++ ramdisk.cc ../lib/libblksrv.a -o ramdisk -I../include -pthread

../lib/libblksrv.a :
	make -C ..
