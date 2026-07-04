#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "launcher.h"

/* ---- Tabla de procesos (TDA) ----
   Guarda el PID y el estado de cada ventana creada, para poder
   consultarlo desde el menu sin tener que bloquear el programa
   esperando a que las ventanas terminen (a diferencia de la version
   anterior, que hacia wait() de todas las ventanas antes de poder
   volver a mostrar el menu). */

typedef enum
{
    PROC_EJECUTANDO,
    PROC_TERMINADO
} EstadoProceso;

typedef struct
{
    int idVentana;
    pid_t pid;
    EstadoProceso estado;
} InfoProceso;

static InfoProceso tabla[MAX_VENTANAS];
static int totalRegistrados = 0;
static volatile sig_atomic_t huboProcesoTerminado = 0;

static void manejadorSigchld(int sig)
{
    (void)sig;
    huboProcesoTerminado = 1;
}

static void tablaAgregar(int idVentana, pid_t pid)
{
    if (totalRegistrados >= MAX_VENTANAS)
    {
        fprintf(stderr, "No se pueden registrar mas de %d ventanas.\n", MAX_VENTANAS);
        return;
    }

    tabla[totalRegistrados].idVentana = idVentana;
    tabla[totalRegistrados].pid = pid;
    tabla[totalRegistrados].estado = PROC_EJECUTANDO;
    totalRegistrados++;
}

static void tablaMarcarTerminado(pid_t pid)
{
    int i;

    for (i = 0; i < totalRegistrados; i++)
    {
        if (tabla[i].pid == pid)
        {
            tabla[i].estado = PROC_TERMINADO;
            return;
        }
    }
}

/* WNOHANG: si no hay ningun hijo terminado todavia, waitpid regresa
   de inmediato en vez de bloquear el programa. Asi el menu puede
   seguir respondiendo mientras las ventanas siguen abiertas. */
static void revisarProcesosTerminados(void)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        tablaMarcarTerminado(pid);
        printf("\n[launcher] La ventana con PID %d termino.\n", (int)pid);
    }
}

static void tablaImprimir(void)
{
    int i;

    if (totalRegistrados == 0)
    {
        printf("  (todavia no se ha creado ninguna ventana)\n");
        return;
    }

    printf("  %-10s %-10s %s\n", "Ventana", "PID", "Estado");

    for (i = 0; i < totalRegistrados; i++)
    {
        printf("  %-10d %-10d %s\n",
               tabla[i].idVentana,
               (int)tabla[i].pid,
               tabla[i].estado == PROC_EJECUTANDO ? "EJECUTANDO" : "TERMINADO");
    }
}

static void terminarTodas(void)
{
    int i;

    for (i = 0; i < totalRegistrados; i++)
    {
        if (tabla[i].estado == PROC_EJECUTANDO)
        {
            kill(tabla[i].pid, SIGTERM);
        }
    }
}

/* Construye la ruta del ejecutable "window" a partir de la ubicacion
   del propio launcher (leyendo /proc/self/exe), en vez de usar una
   ruta relativa fija como "./bin/window". Asi el programa funciona
   sin importar desde que carpeta se ejecute el launcher. */
static int obtenerRutaWindow(char *buffer, size_t tam)
{
    ssize_t n = readlink("/proc/self/exe", buffer, tam - 1);
    char *ultimoSlash;
    size_t dirLen;
    const char *nombreWindow = "/window";

    if (n < 0)
    {
        perror("readlink");
        return -1;
    }

    buffer[n] = '\0';

    ultimoSlash = strrchr(buffer, '/');

    if (!ultimoSlash)
    {
        return -1;
    }

    dirLen = (size_t)(ultimoSlash - buffer);

    if (dirLen + strlen(nombreWindow) + 1 > tam)
    {
        return -1;
    }

    snprintf(buffer + dirLen, tam - dirLen, "%s", nombreWindow);
    return 0;
}

static void crearVentana(int idVentana, const char *rutaWindow,
                          const char *host, const char *puerto)
{
    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return;
    }

    if (pid == 0)
    {
        execl(rutaWindow, "window", host, puerto, (char *)NULL);

        /* Si execl regresa, es porque fallo. */
        fprintf(stderr, "Error ejecutando window: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    printf("Launcher: ventana %d creada, PID=%d\n", idVentana, (int)pid);
    tablaAgregar(idVentana, pid);
}

void crearVentanas(int cantidad, const char *host, const char *puerto)
{
    static int siguienteId = 1;
    char rutaWindow[PATH_MAX];
    int i;

    if (obtenerRutaWindow(rutaWindow, sizeof(rutaWindow)) != 0)
    {
        fprintf(stderr, "No se pudo determinar la ruta del programa window.\n");
        return;
    }

    if (cantidad > MAX_VENTANAS - totalRegistrados)
    {
        printf("Solo se pueden crear %d ventanas mas (limite: %d).\n",
               MAX_VENTANAS - totalRegistrados, MAX_VENTANAS);
        cantidad = MAX_VENTANAS - totalRegistrados;
    }

    for (i = 0; i < cantidad; i++)
    {
        crearVentana(siguienteId, rutaWindow, host, puerto);
        siguienteId++;
    }
}

/* Lee una linea completa de stdin, sin dejar el salto de linea final
   ni caracteres sobrantes en el buffer de entrada -- a diferencia de
   scanf("%d", ...), que ante una entrada invalida (por ejemplo una
   letra) puede dejar el programa repitiendo el mensaje de error para
   siempre sin poder recuperarse. */
static int leerLinea(char *buffer, size_t tam)
{
    if (!fgets(buffer, (int)tam, stdin))
    {
        return -1;
    }

    buffer[strcspn(buffer, "\n")] = '\0';
    return 0;
}

/* Pide un numero entero dentro de [minimo, maximo], repitiendo la
   pregunta mientras la entrada no sea valida. Devuelve -1 si la
   entrada se cerro (por ejemplo con Ctrl+D). */
static int leerEnteroPositivo(const char *mensaje, int minimo, int maximo)
{
    char linea[32];
    char *fin;
    long valor;

    while (1)
    {
        printf("%s", mensaje);
        fflush(stdout);

        if (leerLinea(linea, sizeof(linea)) != 0)
        {
            return -1;
        }

        valor = strtol(linea, &fin, 10);

        if (linea[0] == '\0' || *fin != '\0')
        {
            printf("  Entrada invalida, escribe solo un numero.\n");
            continue;
        }

        if (valor < minimo || valor > maximo)
        {
            printf("  El valor debe estar entre %d y %d.\n", minimo, maximo);
            continue;
        }

        return (int)valor;
    }
}

void mostrarMenu(const char *host, const char *puerto)
{
    int opcion;
    int cantidad;
    int salir = 0;
    struct sigaction sa;

    /* Nos avisamos con SIGCHLD cada vez que una ventana termina, en
       vez de bloquear el programa esperandolas con wait(). Asi el
       menu sigue respondiendo aunque haya ventanas abiertas. */
    sa.sa_handler = manejadorSigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NOCLDSTOP;

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
    {
        perror("sigaction");
        return;
    }

    printf("Las ventanas se conectaran a IALearner en %s:%s\n", host, puerto);

    while (!salir)
    {
        if (huboProcesoTerminado)
        {
            huboProcesoTerminado = 0;
            revisarProcesosTerminados();
        }

        printf("\n========== LAUNCHER ==========\n");
        printf("1. Crear ventanas\n");
        printf("2. Ver estado de procesos\n");
        printf("3. Terminar todas las ventanas activas\n");
        printf("0. Salir\n");
        printf("==============================\n");

        opcion = leerEnteroPositivo("Seleccione: ", 0, 3);

        if (opcion < 0)
        {
            /* Entrada cerrada (Ctrl+D): salimos con cuidado. */
            salir = 1;
            continue;
        }

        switch (opcion)
        {
            case 1:
                cantidad = leerEnteroPositivo("Cantidad de ventanas: ", 1, MAX_VENTANAS);
                if (cantidad > 0)
                {
                    crearVentanas(cantidad, host, puerto);
                }
                break;

            case 2:
                revisarProcesosTerminados();
                tablaImprimir();
                break;

            case 3:
                terminarTodas();
                break;

            case 0:
                printf("Adios.\n");
                salir = 1;
                break;

            default:
                break;
        }
    }

    /* Al salir, nos aseguramos de no dejar ventanas huerfanas
       corriendo sin que nadie las este esperando. */
    terminarTodas();

    {
        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, 0)) > 0)
        {
            tablaMarcarTerminado(pid);
        }
    }
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    const char *puerto = "5000";

    /* Permite indicar host y puerto del data center como argumentos:
       ./launcher [host] [puerto]. Si no se dan, se usan los valores
       por defecto (solo para pruebas locales). */
    if (argc >= 2)
    {
        host = argv[1];
    }
    if (argc >= 3)
    {
        puerto = argv[2];
    }

    mostrarMenu(host, puerto);

    return 0;
}
