CC = gcc
CFLAGS = -g -Wall -Wno-parentheses -D_REENTRANT
LDFLAGS = -lpthread

web_server: server.c util.o
	${CC} ${CFLAGS} -o web_server server.c util.o ${LDFLAGS}

util.o: util.c util.h
	${CC} ${CFLAGS} -o util.o -c util.c

clean:
	rm web_server


