CC = gcc

CFLAGS = -O2 -Wall -Wextra

SRC = lab_3.c
OUT = udp_test

all: $(OUT)

$(OUT): $(SRC)
	$(CC) $(CFLAGS) -o $(OUT) $(SRC)

run: $(OUT)
	./$(OUT)

runjava:
	javac UdpBroadcastSender.java && java UdpBroadcastSender

killudp:
	sudo kill -9 $(sudo lsof -t -iUDP:9999)

clean:
	rm -f $(OUT)