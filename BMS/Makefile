CC     = gcc
CFLAGS = -Wall -Wextra -g

SERVER_SRC = server.c auth.c customer.c staff.c employee.c manager.c admin.c role.c utils.c
HEADERS    = common.h utils.h auth.h customer.h staff.h employee.h manager.h admin.h role.h

all: server client main

server: $(SERVER_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o server $(SERVER_SRC)

client: client.c common.h
	$(CC) $(CFLAGS) -o client client.c

main: main.c
	$(CC) $(CFLAGS) -o main main.c

clean:
	rm -f server client main
