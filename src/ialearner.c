#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "ialearner.h"
#include "classifier.h"

#define PUERTO_DEFECTO "5000"
#define TAMANO_BUFFER 1024

int documentosCorreo = 0;
int documentosArticulo = 0;
int documentosReporte = 0;
int clientesConectados = 0;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Cada hilo atiende UNA ventana. Va acumulando en "documento" la
   suma de frecuencias de todas las lineas que llegan de esa ventana
   (una linea = lo que el usuario escribio antes de presionar Enter).
   La clasificacion se hace UNA sola vez, cuando la conexion se
   cierra -- es decir, cuando el proceso (ventana) termina, tal como
   pide el enunciado -- en vez de clasificar cada linea por separado
   como una version anterior de este programa hacia. */
void *atenderCliente(void *arg)
{
    int cliente = *(int *)arg;
    char c;
    char linea[TAMANO_BUFFER];
    int indice = 0;
    ssize_t leidos;
    DocumentoVentana documento;
    int tipo;

    free(arg);

    documentoInicializar(&documento);

    printf("Nuevo hilo creado para cliente.\n");
    fflush(stdout);

    while ((leidos = recv(cliente, &c, 1, 0)) > 0)
    {
        if (c == '\n')
        {
            linea[indice] = '\0';
            printf("[ialearner] linea recibida: %s\n", linea);

            documentoAgregarLinea(&documento, linea);

            indice = 0;
        }
        else if (indice < (int)sizeof(linea) - 1)
        {
            linea[indice++] = c;
        }
        /* Si una linea excede el buffer sin traer '\n', el exceso se
           descarta en vez de desbordar memoria. */
    }

    if (leidos < 0)
    {
        perror("recv");
    }

    /* Si la ventana se cerro con texto pendiente (el usuario escribio
       algo pero nunca presiono Enter), no se pierde: se cuenta igual
       que una linea mas del documento. */
    if (indice > 0)
    {
        linea[indice] = '\0';
        printf("[ialearner] linea final (sin Enter): %s\n", linea);
        documentoAgregarLinea(&documento, linea);
    }

    printf("\n=============================\n");
    printf("Ventana finalizada. Clasificando documento completo...\n\n");

    documentoImprimirResumen(&documento);

    tipo = documentoClasificar(&documento);
    actualizarResumen(tipo);

    printf("\nClasificacion final de la ventana: %s\n", nombreClase(tipo));
    printf("=============================\n\n");

    printf("Cliente desconectado.\n");
    fflush(stdout);

    pthread_mutex_lock(&mutex);
    clientesConectados--;

    if (clientesConectados == 0)
    {
        mostrarResumenFinal();
    }

    pthread_mutex_unlock(&mutex);

    close(cliente);

    return NULL;
}

void actualizarResumen(int tipo)
{
    pthread_mutex_lock(&mutex);

    if (tipo == EMAIL)
    {
        documentosCorreo++;
    }
    else if (tipo == ARTICULO)
    {
        documentosArticulo++;
    }
    else if (tipo == REPORTE)
    {
        documentosReporte++;
    }

    pthread_mutex_unlock(&mutex);
}

/* NOTA: esta funcion se llama cada vez que el numero de ventanas
   conectadas vuelve a 0. Si el usuario crea ventanas en varios lotes
   desde el menu del launcher (opcion 1 mas de una vez), esto puede
   imprimirse mas de una vez durante la sesion -- cada vez mostrando
   los totales acumulados hasta ese momento. El ultimo resumen
   impreso antes de cerrar ialearner es el que refleja el total real
   de toda la sesion. */
void mostrarResumenFinal(void)
{
    int total = documentosCorreo + documentosArticulo + documentosReporte;
    double pCorreo, pArticulo, pReporte;

    printf("\n=============================\n");
    printf("RESUMEN ACUMULADO\n\n");

    printf("Correo electronico : %d\n", documentosCorreo);
    printf("Articulo cientifico: %d\n", documentosArticulo);
    printf("Reporte            : %d\n", documentosReporte);

    if (total == 0)
    {
        printf("No hay documentos clasificados todavia.\n");
        printf("=============================\n\n");
        return;
    }

    pCorreo = (double)documentosCorreo / total;
    pArticulo = (double)documentosArticulo / total;
    pReporte = (double)documentosReporte / total;

    printf("\nProporciones\n");
    printf("Correo   : %.2f%%\n", pCorreo * 100);
    printf("Articulo : %.2f%%\n", pArticulo * 100);
    printf("Reporte  : %.2f%%\n", pReporte * 100);

    printf("\nTipo de usuario: ");

    if (documentosArticulo == 0 && documentosReporte == 0)
    {
        printf("Personal administrativo");
    }
    else if (pCorreo <= pArticulo && pCorreo <= pReporte)
    {
        printf("Estudiante");
    }
    else if (pArticulo <= pCorreo && pArticulo <= pReporte)
    {
        printf("Personal tecnico");
    }
    else
    {
        printf("Profesor");
    }

    printf("\n=============================\n\n");
}

static int crearSocketEscucha(const char *puerto)
{
    int servidor;
    struct sockaddr_in direccion;
    int reutilizar = 1;
    int numeroPuerto = atoi(puerto);

    if (numeroPuerto <= 0 || numeroPuerto > 65535)
    {
        fprintf(stderr, "Puerto invalido: %s\n", puerto);
        return -1;
    }

    servidor = socket(AF_INET, SOCK_STREAM, 0);

    if (servidor < 0)
    {
        perror("socket");
        return -1;
    }

    /* Evita el error "Address already in use" si se reinicia el
       servidor justo despues de cerrarlo. */
    if (setsockopt(servidor, SOL_SOCKET, SO_REUSEADDR, &reutilizar, sizeof(reutilizar)) < 0)
    {
        perror("setsockopt");
        close(servidor);
        return -1;
    }

    direccion.sin_family = AF_INET;
    direccion.sin_port = htons((unsigned short)numeroPuerto);
    direccion.sin_addr.s_addr = INADDR_ANY;

    if (bind(servidor, (struct sockaddr *)&direccion, sizeof(direccion)) < 0)
    {
        perror("bind");
        close(servidor);
        return -1;
    }

    if (listen(servidor, 16) < 0)
    {
        perror("listen");
        close(servidor);
        return -1;
    }

    return servidor;
}

int main(int argc, char *argv[])
{
    const char *puerto = PUERTO_DEFECTO;
    int servidor;

    if (argc >= 2)
    {
        puerto = argv[1];
    }

    /* Si una ventana se desconecta justo cuando le intentamos enviar
       algo, el sistema manda SIGPIPE, que por defecto mata el
       proceso completo. Este servidor no le escribe nada al cliente
       todavia, pero se ignora la señal para no arrastrar un bug
       silencioso si eso cambia mas adelante. */
    signal(SIGPIPE, SIG_IGN);

    servidor = crearSocketEscucha(puerto);

    if (servidor < 0)
    {
        return EXIT_FAILURE;
    }

    printf("IA Learner escuchando en el puerto %s.\n", puerto);
    fflush(stdout);

    while (1)
    {
        int *cliente = malloc(sizeof(int));
        pthread_t hilo;

        if (!cliente)
        {
            fprintf(stderr, "Sin memoria para atender una nueva conexion.\n");
            continue;
        }

        *cliente = accept(servidor, NULL, NULL);

        if (*cliente < 0)
        {
            perror("accept");
            free(cliente);
            continue;
        }

        printf("Nueva conexion.\n");

        pthread_mutex_lock(&mutex);
        clientesConectados++;
        pthread_mutex_unlock(&mutex);

        if (pthread_create(&hilo, NULL, atenderCliente, cliente) != 0)
        {
            perror("pthread_create");

            pthread_mutex_lock(&mutex);
            clientesConectados--;
            pthread_mutex_unlock(&mutex);

            close(*cliente);
            free(cliente);
            continue;
        }

        /* No esperamos (join) a este hilo: en cuanto termina, el
           sistema libera solo sus recursos. Si lo dejaramos
           "joinable" sin hacer join nunca, esos recursos quedarian
           reservados indefinidamente -- el equivalente, en hilos, a
           un proceso zombie. */
        pthread_detach(hilo);
    }

    close(servidor);

    return 0;
}
