#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

/* Valores por defecto: solo se usan si nadie pasa host/puerto como
   argumento al ejecutar el programa. Nunca se usan "quemados"
   directamente dentro de conectarServidor(). */
#define SERVER_IP_DEFECTO   "127.0.0.1"
#define SERVER_PORT_DEFECTO "5000"

/* Abre una conexion TCP hacia host:puerto.
   Devuelve el descriptor de socket (>=0) si tuvo exito, o -1 si hubo
   error */
int conectarServidor(const char *host, const char *puerto);

#endif
