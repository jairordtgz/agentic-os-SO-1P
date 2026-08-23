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

#define CAPACIDAD_INICIAL 8
#define UMBRAL_ADVERTENCIA 100

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

static InfoProceso *tabla = NULL;
static int totalRegistrados = 0;
static int capacidadTabla = 0;
static volatile sig_atomic_t huboProcesoTerminado = 0;

static void manejadorSigchld(int sig)
{
    (void)sig;
    huboProcesoTerminado = 1;
}

static int tablaAgregar(int idVentana, pid_t pid)
{
    if (totalRegistrados == capacidadTabla)
    {
        int nuevaCapacidad = (capacidadTabla == 0) ? CAPACIDAD_INICIAL : capacidadTabla * 2;
        InfoProceso *nuevaTabla = realloc(tabla, sizeof(InfoProceso) * (size_t)nuevaCapacidad);

        if (!nuevaTabla)
        {
            fprintf(stderr, "Sin memoria para registrar mas ventanas.\n");
            return -1;
        }

        tabla = nuevaTabla;
        capacidadTabla = nuevaCapacidad;
    }

    tabla[totalRegistrados].idVentana = idVentana;
    tabla[totalRegistrados].pid = pid;
    tabla[totalRegistrados].estado = PROC_EJECUTANDO;
    totalRegistrados++;
    return 0;
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

/* Libera la memoria de la tabla dinamica al terminar el programa. */
static void tablaLiberar(void)
{
    free(tabla);
    tabla = NULL;
    totalRegistrados = 0;
    capacidadTabla = 0;
}

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
    pid_t idLauncher = getpid();
    pid_t pid = fork();

    if (pid < 0)
    {
        perror("fork");
        return;
    }

    if (pid == 0)
    {
        char idVentanaStr[16];
        char idLauncherStr[16];

        setpgid(0, 0);

        /* Se le pasan idVentana e idLauncher como argumentos para que
           window.c pueda identificarse ante ialearner con los mismos
           numeros que el usuario ve en el launcher */

        snprintf(idVentanaStr, sizeof(idVentanaStr), "%d", idVentana);
        snprintf(idLauncherStr, sizeof(idLauncherStr), "%d", (int)idLauncher);

        execl(rutaWindow, "window", host, puerto, idVentanaStr, idLauncherStr, (char *)NULL);

        fprintf(stderr, "Error ejecutando window: %s\n", strerror(errno));
        exit(EXIT_FAILURE);
    }

    printf("Launcher: ventana %d creada, PID=%d\n", idVentana, (int)pid);

    if (tablaAgregar(idVentana, pid) != 0)
    {
        fprintf(stderr,
                "Aviso: la ventana %d (PID %d) quedo sin registrar en la tabla.\n",
                idVentana, (int)pid);
    }
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

    for (i = 0; i < cantidad; i++)
    {
        crearVentana(siguienteId, rutaWindow, host, puerto);
        siguienteId++;
    }
}

static int leerLinea(char *buffer, size_t tam)
{
    if (!fgets(buffer, (int)tam, stdin))
    {
        return -1;
    }

    buffer[strcspn(buffer, "\n")] = '\0';
    return 0;
}

/* Lee una linea de texto */

static void leerTexto(const char *mensaje, const char *porDefecto, char *destino, size_t tam)
{
    char linea[128];

    printf("%s", mensaje);
    fflush(stdout);

    if (leerLinea(linea, sizeof(linea)) != 0 || linea[0] == '\0')
    {
        snprintf(destino, tam, "%s", porDefecto);
        return;
    }

    snprintf(destino, tam, "%s", linea);
}
// permite ingresar solo numeros positivos en launcher

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

/* muestra al usuario advertencia de que si esta seguro crear mas de 100 ventanas */
static int confirmarCantidadGrande(int cantidad)
{
    char respuesta[8];

    if (cantidad <= UMBRAL_ADVERTENCIA)
    {
        return 1;
    }

    printf("Vas a crear %d ventanas; esto puede consumir muchos recursos.\n", cantidad);
    printf("¿Continuar? (s/n): ");
    fflush(stdout);

    if (leerLinea(respuesta, sizeof(respuesta)) != 0)
    {
        return 0;
    }

    return (respuesta[0] == 's' || respuesta[0] == 'S');
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
    sa.sa_flags = SA_NOCLDSTOP | SA_RESTART;

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
    {
        perror("sigaction");
        return;
    }

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
            salir = 1;
            continue;
        }

        switch (opcion)
        {
            case 1:
            {
                char hostLote[128];
                char puertoLote[16];

                cantidad = leerEnteroPositivo("Cantidad de ventanas: ", 1, INT_MAX);

                if (cantidad <= 0)
                {
                    break;
                }

                leerTexto("Host de ialearner [Enter para usar el de por defecto]: ",
                          host, hostLote, sizeof(hostLote));
                leerTexto("Puerto de la computadora [Enter para usar el de por defecto]: ",
                          puerto, puertoLote, sizeof(puertoLote));

                if (confirmarCantidadGrande(cantidad))
                {
                    printf("Creando %d ventana(s) hacia %s:%s...\n", cantidad, hostLote, puertoLote);
                    crearVentanas(cantidad, hostLote, puertoLote);
                }
                else
                {
                    printf("Operacion cancelada.\n");
                }
                break;
            }

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

    /* Al salir, nos aseguramos de no dejar ventanas huerfanas */
    terminarTodas();

    {
        int status;
        pid_t pid;

        while ((pid = waitpid(-1, &status, 0)) > 0)
        {
            tablaMarcarTerminado(pid);
        }
    }

    tablaLiberar();
}

int main(int argc, char *argv[])
{
	// se usa este host y puerto en caso de que el usuario ingrese enter en host y puerto
    const char *host = "127.0.0.1";
    const char *puerto = "5000";

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
