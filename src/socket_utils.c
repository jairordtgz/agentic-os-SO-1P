#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "socket_utils.h"

int conectarServidor(const char *host, const char *puerto)
{
    int socketCliente;
    struct sockaddr_in servidor;
    struct in_addr direccionIP;
    int numeroPuerto;

    socketCliente = socket(AF_INET, SOCK_STREAM, 0);

    if (socketCliente < 0)
    {
        perror("socket");
        return -1;
    }

    numeroPuerto = atoi(puerto);

    if (numeroPuerto <= 0 || numeroPuerto > 65535)
    {
        fprintf(stderr, "Puerto invalido: %s\n", puerto);
        close(socketCliente);
        return -1;
    }

// determinar si un host/ip es valido

    if (inet_pton(AF_INET, host, &direccionIP) != 1)
    {
        fprintf(stderr, "Direccion IP invalida: %s\n", host);
        close(socketCliente);
        return -1;
    }

    servidor.sin_family = AF_INET;
    servidor.sin_port = htons((unsigned short)numeroPuerto);
    servidor.sin_addr = direccionIP;

    if (connect(socketCliente, (struct sockaddr *)&servidor, sizeof(servidor)) < 0)
    {
        perror("connect");
        close(socketCliente);
        return -1;
    }

    return socketCliente;
}
