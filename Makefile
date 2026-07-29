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
	@pids=$$(sudo lsof -t -iUDP:$(PORT)); \
	if [ -n "$$pids" ]; then \
		sudo kill -9 $$pids && echo "[+] Proceso(s) $$pids terminados (UDP:$(PORT))"; \
	else \
		echo "[!] No hay nada escuchando en UDP:$(PORT)"; \
	fi

clean:
	rm -f $(OUT) dns_server.log