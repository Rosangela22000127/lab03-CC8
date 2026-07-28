CC = gcc

CFLAGS = -O2 -Wall -Wextra -pthread

SRC = lab_3.c
OUT = dns_server
PORT ?= 53

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

run: $(OUT)
	./$(OUT) $(PORT)

runjava:
	javac UdpBroadcastSender.java && java UdpBroadcastSender

killudp:
	sudo kill -9 $(sudo lsof -t -iUDP:$(PORT))

clean:
	rm -f $(OUT) dns_server.log