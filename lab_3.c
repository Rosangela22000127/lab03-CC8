#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define BUFFER_SIZE 65535 // Limite de UDP, Optimice
#define LISTEN_PORT 9999  // Puedes cambiarlo si deseas otro puerto, Default Broadcast

// Función para recibir paquetes UDP
void start_udp_listener() {
    int sockfd;
    struct sockaddr_in servaddr, cliaddr;
    char buffer[BUFFER_SIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Listener: [-] Error al crear el socket");
        exit(EXIT_FAILURE);
    }

    // Permitir reutilización del puerto
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Listener: [-] Error en setsockopt");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(LISTEN_PORT);

    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("Listener: [-] Error en bind");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Listener: [+] Escuchando en puerto UDP %d...\n", LISTEN_PORT);

    socklen_t len = sizeof(cliaddr);
    
    while (1) {
        ssize_t n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr *)&cliaddr, &len);
        if (n < 0) {
            perror("Listener: [-] Error al recibir datos");
            continue;
        }

        buffer[n] = '\0';
        printf("\nListener: [+] Paquete recibido de %s:%d\n", inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));
        printf("Listener: [>] Datos: %s\n", buffer);
    }

    close(sockfd);
}

// Función para enviar un paquete UDP
void send_udp_packet(const char *ip, int port, const char *message) {
    int sockfd;
    struct sockaddr_in dest;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Sender: [-] Error al crear socket para envío");
        return;
    }

    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    inet_pton(AF_INET, ip, &dest.sin_addr);

    sendto(sockfd, message, strlen(message), 0, (struct sockaddr *)&dest, sizeof(dest));
    printf("Sender: [+] Paquete enviado a %s:%d\n", ip, port);

    close(sockfd);
}

int main() {
    pid_t pid = fork();

    if (pid == 0) {
        // Escucha
        start_udp_listener();
    } else {
        // Un Paquete de Prueba
        sleep(5); // Espera breve para que el listener esté listo
        printf("Main: Enviando UDP con DATA\n");
        send_udp_packet("127.0.0.1", LISTEN_PORT, "Hola desde LAB03 UDP");
    }

    return 0;
}