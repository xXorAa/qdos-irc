BIN = qirc
CC = qcc

CFLAGS = 
LDFLAGS = -lcurses -lsocket

PREFIX=/usr/local

all: ${BIN}

qirc : irc.c
	${CC} -o qirc irc.c ${LDFLAGS}

install: ${BIN}
	mkdir -p ${PREFIX}/bin
	cp -f ${BIN} ${PREFIX}/bin
	chmod 755 ${PREFIX}/bin/${BIN}

uninstall:
	rm -f ${PREFIX}/bin/${BIN}

clean:
	rm -f ${BIN} *.o

.PHONY: all clean

deploy: ${BIN}
	curl -o qirc.zip http://www.dilwyn.me.uk/pe/ptrgen206.zip
	qlzip -Q2 qirc.zip qirc
	curl -O http://www.dilwyn.me.uk/tk/sigext30mod.zip
	unzip sigext30mod.zip sigext30.rext
	curl -O http://www.dilwyn.me.uk/tk/env.zip
	unzip env.zip env.bin
	curl -O http://www.dilwyn.me.uk/c/cursesex.zip
	unzip cursesex.zip terminfo.QDOS
	qlzip qirc.zip BOOT sigext30.rext env.bin terminfo.QDOS
	-rm sigext30.rext sigext30mod.zip env.bin env.zip cursesex.zip terminfo.QDOS
