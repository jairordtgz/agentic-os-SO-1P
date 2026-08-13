#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <limits.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "ialearner.h"
#include "classifier.h"

#define PUERTO_DEFECTO "5000"
#define CARPETA_DICCIONARIOS_DEFECTO "diccionarios"
#define TAMANO_BUFFER 1024

int documentosCorreo = 0;
int documentosArticulo = 0;
int documentosReporte = 0;
int clientesConectados = 0;

DocumentoVentana matrizGlobal;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* Cada hilo atiende UNA ventana. Va acumulando en "documento" la
   suma de frecuencias de todas las lineas que llegan de esa ventana
   (una linea = lo que el usuario escribio antes de presionar Enter).
   La clasificacion se hace UNA sola vez, cuando la conexion se
   cierra -- es decir, cuando el proceso (ventana) termina, tal como
   pide el enunciado -- en vez de clasificar cada linea por separado
   como una version anterior de este programa hacia.

   ADEMAS de su "documento" local, cada hilo tambien va sumando cada
   linea a "matrizGlobal", que es la UNICA estructura de este archivo
   que de verdad comparten todos los hilos (todas las ventanas
   escriben en la misma matriz al mismo tiempo). Por eso, y solo por
   eso, esa actualizacion va protegida con mutex -- "documento" es
   privado de este hilo y no lo necesita. */
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

            /* Documento LOCAL de este hilo: nadie mas lo toca. */
            documentoAgregarLinea(&documento, linea);

            /* Matriz GLOBAL, compartida por todos los hilos: aqui
               si hace falta el mutex, porque dos hilos podrian estar
               incrementando la misma posicion del vector al mismo
               tiempo si no se protege. */
            pthread_mutex_lock(&mutex);
            documentoAgregarLinea(&matrizGlobal, linea);
            pthread_mutex_unlock(&mutex);

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
       que una linea mas del documento (local y global por igual). */
    if (indice > 0)
    {
        linea[indice] = '\0';
        printf("[ialearner] linea final (sin Enter): %s\n", linea);

        documentoAgregarLinea(&documento, linea);

        pthread_mutex_lock(&mutex);
        documentoAgregarLinea(&matrizGlobal, linea);
        pthread_mutex_unlock(&mutex);
    }

    printf("\n=============================\n");
    printf("Ventana finalizada. Clasificando documento completo...\n\n");

    documentoImprimirResumen(&documento);

    tipo = documentoClasificar(&documento);
    actualizarResumen(tipo);

    printf("\nClasificacion final de la ventana: %s\n", nombreClase(tipo));
    printf("=============================\n\n");
    fflush(stdout);

    printf("Cliente desconectado.\n");
    fflush(stdout);

    pthread_mutex_lock(&mutex);
    clientesConectados--;

    if (clientesConectados == 0)
    {
        mostrarResumenFinal();
    }

    pthread_mutex_unlock(&mutex);

    documentoLiberar(&documento);

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
   de toda la sesion.

   IMPORTANTE: esta funcion se llama SIEMPRE con el mutex ya tomado
   por quien la invoca (ver atenderCliente). Por eso lee "matrizGlobal"
   directamente, SIN volver a hacer lock aqui adentro -- si lo
   hiciera, el mismo hilo intentaria tomar un mutex que el ya tiene
   tomado, y el programa se quedaria congelado esperandose a si mismo
   (esto se llama deadlock). */
void mostrarResumenFinal(void)
{
    int total = documentosCorreo + documentosArticulo + documentosReporte;
    double pCorreo, pArticulo, pReporte;

    printf("\n=============================\n");
    printf("RESUMEN ACUMULADO\n\n");

    printf("Correo electronico : %d\n", documentosCorreo);
    printf("Articulo cientifico: %d\n", documentosArticulo);
    printf("Reporte            : %d\n", documentosReporte);

    printf("\nMatriz global (todas las ventanas combinadas, en vivo):\n");
    documentoImprimirResumen(&matrizGlobal);

    if (total == 0)
    {
        printf("\nNo hay documentos clasificados todavia.\n");
        printf("=============================\n\n");
        fflush(stdout);
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
    fflush(stdout);
}

/* Construye la ruta de la carpeta "diccionarios" a partir de la
   ubicacion del propio ejecutable (leyendo /proc/self/exe), en vez
   de asumir que se ejecuta desde la raiz del proyecto. El binario
   vive en bin/, y diccionarios/ vive un nivel arriba -- por eso el
   sufijo es "/../diccionarios". Asi ialearner encuentra sus
   diccionarios sin importar desde que carpeta lo ejecutes. */
static int obtenerCarpetaDiccionariosPorDefecto(char *buffer, size_t tam)
{
    ssize_t n = readlink("/proc/self/exe", buffer, tam - 1);
    char *ultimoSlash;
    size_t dirLen;
    const char *sufijo = "/../diccionarios";

    if (n < 0)
    {
        return -1;
    }

    buffer[n] = '\0';

    ultimoSlash = strrchr(buffer, '/');

    if (!ultimoSlash)
    {
        return -1;
    }

    dirLen = (size_t)(ultimoSlash - buffer);

    if (dirLen + strlen(sufijo) + 1 > tam)
    {
        return -1;
    }

    snprintf(buffer + dirLen, tam - dirLen, "%s", sufijo);
    return 0;
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
    const char *carpetaDiccionarios;
    char carpetaResuelta[PATH_MAX];
    int servidor;

    if (obtenerCarpetaDiccionariosPorDefecto(carpetaResuelta, sizeof(carpetaResuelta)) == 0)
    {
        carpetaDiccionarios = carpetaResuelta;
    }
    else
    {
        carpetaDiccionarios = CARPETA_DICCIONARIOS_DEFECTO;
    }

    if (argc >= 2)
    {
        puerto = argv[1];
    }
    if (argc >= 3)
    {
        /* Si el usuario indica la carpeta explicitamente, esa gana
           sobre la ruta resuelta automaticamente. */
        carpetaDiccionarios = argv[2];
    }

    /* Los diccionarios se cargan UNA sola vez aqui, ANTES de aceptar
       cualquier conexion (todavia no existe ningun hilo). Por eso la
       tabla hash de cada diccionario no necesita mutex: se termina
       de escribir por completo antes de que el primer hilo pueda
       siquiera empezar a leerla. */
    if (cargarDiccionarios(carpetaDiccionarios) != 0)
    {
        fprintf(stderr, "No se pudieron cargar los diccionarios desde '%s'.\n", carpetaDiccionarios);
        return EXIT_FAILURE;
    }

    documentoInicializar(&matrizGlobal);

    /* Si una ventana se desconecta justo cuando le intentamos enviar
       algo, el sistema manda SIGPIPE, que por defecto mata el
       proceso completo. Este servidor no le escribe nada al cliente
       todavia, pero se ignora la señal para no arrastrar un bug
       silencioso si eso cambia mas adelante. */
    signal(SIGPIPE, SIG_IGN);

    servidor = crearSocketEscucha(puerto);

    if (servidor < 0)
    {
        liberarDiccionarios();
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
    documentoLiberar(&matrizGlobal);
    liberarDiccionarios();

    return 0;
}
