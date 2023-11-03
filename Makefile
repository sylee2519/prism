.PHONY: all clean
.DEFAULT_GOAL := all

LIBS=-lrt -lm -lstdc++fs -lssl -lcrypto -llustreapi -lpthread
INCLUDES=-./include
CFLAGS=-std=c++17 -O0 -g -fpermissive

MPICXX=mpicxx

OUTPUT = prism

all: main

main: main.cc
	$(MPICXX) $(CFLAGS) -o $(OUTPUT) main.cc prism.cc jc.cc $(LIBS)

clean:
	rm $(OUTPUT)
