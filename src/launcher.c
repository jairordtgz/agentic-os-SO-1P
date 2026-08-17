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
   Guarda el PID y el estado de cada ventana creada. A diferencia de
   una version anterior, esta tabla NO tiene un tope fijo (no existe
   ningun "MAX_VENTANAS"): crece dinamicamente con realloc segun se
   necesite, para que "N ventanas" pueda ser realmente cualquier
   cantidad que la maquina soporte, tal como lo pide el enunciado. */

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

/* Agrega una ventana a la tabla, haciendo crecer el arreglo al doble
   de su capacidad cuando ya no cabe. Devuelve -1 solo si de verdad no
   hay memoria disponible (caso extremo, no un limite artificial). */
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

/* Libera la memoria de la tabla dinamica al terminar el programa. */
static void tablaLiberar(void)
{
    free(tabla);
    tabla = NULL;
    totalRegistrados = 0;
    capacidadTabla = 0;
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
        /* Cada ventana pasa a formar su propio grupo de procesos.
           Sin esto, heredaria el mismo grupo que el launcher y la
           terminal, y un Ctrl+C hecho en la terminal del launcher
           (SIGINT) se propagaria a TODAS las ventanas a la vez,
           cerrandolas todas de golpe en vez de solo al launcher. */
        setpgid(0, 0);

        execl(rutaWindow, "window", host, puerto, (char *)NULL);

        /* Si execl regresa, es porque fallo. */
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

/* Crea "cantidad" ventanas nuevas. No existe un tope maximo propio
   del programa: el unico limite real es lo que la maquina pueda
   soportar (procesos, memoria, conexiones de X11). */
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

/* Lee una linea de texto; si el usuario deja la entrada vacia
   (solo presiona Enter), se usa "porDefecto" en su lugar. Se usa
   para pedir el host/puerto de la "computadora remota" a la que se
   conectara un lote de ventanas, sin obligar a escribirlo cada vez. */
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

/* Si el usuario pide una cantidad muy grande de ventanas, se le avisa
   y se le pide confirmar. No es un limite del programa, es solo una
   proteccion contra errores de tecleo (ej. escribir de mas un cero),
   ya que crear miles de procesos de golpe puede saturar la maquina. */
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
            {
                /* Sin tope maximo propio: solo INT_MAX como limite
                   tecnico del tipo "int", no una regla de negocio. */
                char hostLote[128];
                char puertoLote[16];

                cantidad = leerEnteroPositivo("Cantidad de ventanas: ", 1, INT_MAX);

                if (cantidad <= 0)
                {
                    break;
                }

                /* Cada lote de ventanas puede apuntar a una
                   "computadora remota" distinta (un ialearner en
                   otro puerto, simulando otra maquina) -- si el
                   usuario solo presiona Enter, se usa el host/puerto
                   por defecto de esta sesion del launcher. */
                leerTexto("Host de la computadora remota [Enter para usar el de por defecto]: ",
                          host, hostLote, sizeof(hostLote));
                leerTexto("Puerto de la computadora remota [Enter para usar el de por defecto]: ",
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

    tablaLiberar();
}

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    const char *puerto = "5000";

    /* Permite indicar host y puerto del data center como argumentos:
       ./launcher [host] [puerto]. Estos valores solo se usan si el
       usuario NO especifica nada -- por lo tanto no estan "quemados":
       cualquiera puede cambiarlos sin tocar ni recompilar el codigo. */
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
