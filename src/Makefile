C++ = g++

ifndef os
   UNAME_S := $(shell uname -s)
   ifeq ($(UNAME_S),Darwin)
      os = OSX
   else
      os = LINUX
   endif
endif

ifndef arch
   arch = AMD64
endif

CCFLAGS = -Wall -D$(os) -I../udt/src -finline-functions -g

ifeq ($(arch), IA32)
   CCFLAGS += -DIA32 #-mcpu=pentiumpro -march=pentiumpro -mmmx -msse
endif

ifeq ($(arch), POWERPC)
   CCFLAGS += -mcpu=powerpc
endif

ifeq ($(arch), IA64)
   CCFLAGS += -DIA64
endif

ifeq ($(arch), SPARC)
   CCFLAGS += -DSPARC
endif

LDFLAGS = -L../src ../udt/src/libudt.a -lstdc++ -lpthread -lm -lssl -lcrypto

ifeq ($(os), UNIX)
   LDFLAGS += -lsocket
endif

ifeq ($(os), OSX)
   # Prefer Apple Silicon Homebrew, then Intel Homebrew
   OPENSSL_PREFIX ?= $(shell \
     if [ -d /opt/homebrew/opt/openssl ]; then echo /opt/homebrew/opt/openssl; \
     elif [ -d /usr/local/opt/openssl ]; then echo /usr/local/opt/openssl; \
     fi)
   ifneq ($(OPENSSL_PREFIX),)
      LDFLAGS += -L$(OPENSSL_PREFIX)/lib
      CCFLAGS += -I$(OPENSSL_PREFIX)/include
   endif
endif

ifeq ($(os), SUNOS)
   LDFLAGS += -lrt -lsocket
endif

DIR = $(shell pwd)

APP = version.h udr

all: $(APP)

version.h:
	./version.sh

%.o: %.cpp
	$(C++) $(CCFLAGS) $< -c

udr: udr.o udr_threads.o udr_util.o udr_options.o udr_mtu.o
	$(C++) $^ -o $@ $(LDFLAGS)

clean:
	rm -f *.o $(APP)

install:
	export PATH=$(DIR):$$PATH
