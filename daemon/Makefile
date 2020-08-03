.PHONY: clean

CFLAGS  := -Werror -g
LD      := gcc
LDLIBS  := ${LDLIBS} -lrdmacm -libverbs -lpthread -lrt

APPS    := daemon

all: ${APPS}

daemon: daemon.o
	${LD} -o $@ $^ ${LDLIBS}

clean:
	rm -f *.o ${APPS}

