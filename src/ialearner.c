/* _GNU_SOURCE debe ir ANTES de cualquier #include: es lo que le pide
   a glibc que exponga pthread_setaffinity_np, CPU_SET y CPU_ZERO,
   necesarios para el balanceo de carga entre CPUs (requerimiento 7a).
   Sin esto, esas funciones/macros no estarian declaradas. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#include <sched.h>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "ialearner.h"
#include "classifier.h"

#define PUERTO_DEFECTO "5000"
#define CARPETA_DICCIONARIOS_DEFECTO "diccionarios"
#define TAMANO_BUFFER 1024

/* ================================================================
   Deteccion del entorno: CPUs y tamaño del pool de hilos (P)
   ================================================================ */

static int cantidadCPUs;
static int P;  /* cantidad de hilos de deteccion, parametro -p / --hilos */

/* Detecta cuantos nucleos de CPU tiene ESTA maquina en tiempo de
   ejecucion. Nunca se recibe como parametro. */
static int detectarCantidadCPUs(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    if (n < 1)
    {
        return 1;
    }

    return (int)n;
}

/* Reparte los P hilos de deteccion entre los CPUs disponibles en
   "round robin" (balanceo de carga, requerimiento 7a). */
static void asignarCPU(pthread_t hilo, int indiceHilo)
{
    cpu_set_t conjunto;
    int cpuAsignado = indiceHilo % cantidadCPUs;

    CPU_ZERO(&conjunto);
    CPU_SET(cpuAsignado, &conjunto);

    if (pthread_setaffinity_np(hilo, sizeof(conjunto), &conjunto) != 0)
    {
        fprintf(stderr, "Aviso: no se pudo fijar el hilo %d al CPU %d.\n", indiceHilo, cpuAsignado);
    }
}

/* ================================================================
   Registro por VENTANA (documento acumulado de una ventana especifica)
   ================================================================ */

typedef struct
{
    int idVentana;
    DocumentoVentana documento;
    int clasificacionActual;  /* EMAIL / ARTICULO / REPORTE / DESCONOCIDO */
} RegistroVentana;

/* ================================================================
   Registro por LAUNCHER (= una "computadora"/usuario independiente).
   Cada launcher que se conecta tiene su PROPIO contexto completo:
   sus propias ventanas, sus propios contadores, su propia matriz
   global y su propia decision de tipo de usuario -- nada de esto se
   mezcla entre launchers distintos, tal como se aclaro en clase. */
typedef struct
{
    int idLauncher;

    pthread_mutex_t mutexVentanas;   /* protege "ventanas" de ESTE launcher */
    RegistroVentana *ventanas;
    int totalVentanas;
    int capacidadVentanas;

    pthread_mutex_t mutexContadores; /* protege lo de abajo, de ESTE launcher */
    int documentosCorreo;
    int documentosArticulo;
    int documentosReporte;
    DocumentoVentana matrizGlobal;
    int ultimoTipoUsuarioImpreso;
} RegistroLauncher;

/* Arreglo de PUNTEROS, no de structs por valor: cada RegistroLauncher
   se reserva una sola vez con malloc y JAMAS se mueve de direccion.
   Si fuera un arreglo de structs por valor, hacer crecer el arreglo
   con realloc podria reubicar en memoria un mutex que otro hilo tiene
   tomado en ese preciso instante -- eso es comportamiento indefinido.
   Con punteros, lo unico que realloc mueve es la lista de direcciones,
   nunca el contenido real de cada RegistroLauncher. */
static RegistroLauncher **launchers = NULL;
static int totalLaunchers = 0;
static int capacidadLaunchers = 0;
static pthread_mutex_t mutexLaunchers = PTHREAD_MUTEX_INITIALIZER;

/* Busca el registro de "idLauncher"; si no existe todavia (es la
   primera vez que se conecta una ventana de ese launcher), lo crea
   con su propio contexto vacio. */
static RegistroLauncher *buscarOCrearLauncher(int idLauncher)
{
    int i;
    RegistroLauncher *nuevo;

    pthread_mutex_lock(&mutexLaunchers);

    for (i = 0; i < totalLaunchers; i++)
    {
        if (launchers[i]->idLauncher == idLauncher)
        {
            pthread_mutex_unlock(&mutexLaunchers);
            return launchers[i];
        }
    }

    nuevo = malloc(sizeof(RegistroLauncher));

    if (!nuevo)
    {
        pthread_mutex_unlock(&mutexLaunchers);
        fprintf(stderr, "Sin memoria para registrar el launcher %d.\n", idLauncher);
        return NULL;
    }

    nuevo->idLauncher = idLauncher;
    pthread_mutex_init(&nuevo->mutexVentanas, NULL);
    nuevo->ventanas = NULL;
    nuevo->totalVentanas = 0;
    nuevo->capacidadVentanas = 0;
    pthread_mutex_init(&nuevo->mutexContadores, NULL);
    nuevo->documentosCorreo = 0;
    nuevo->documentosArticulo = 0;
    nuevo->documentosReporte = 0;
    documentoInicializar(&nuevo->matrizGlobal);
    nuevo->ultimoTipoUsuarioImpreso = -1;

    if (totalLaunchers == capacidadLaunchers)
    {
        int nuevaCapacidad = (capacidadLaunchers == 0) ? 4 : capacidadLaunchers * 2;
        RegistroLauncher **nuevoArreglo = realloc(launchers, sizeof(RegistroLauncher *) * (size_t)nuevaCapacidad);

        if (!nuevoArreglo)
        {
            pthread_mutex_unlock(&mutexLaunchers);
            fprintf(stderr, "Sin memoria para registrar el launcher %d.\n", idLauncher);
            free(nuevo);
            return NULL;
        }

        launchers = nuevoArreglo;
        capacidadLaunchers = nuevaCapacidad;
    }

    launchers[totalLaunchers] = nuevo;
    totalLaunchers++;

    printf("Nuevo launcher registrado: id %d (contexto independiente).\n", idLauncher);
    fflush(stdout);

    pthread_mutex_unlock(&mutexLaunchers);

    return nuevo;
}

/* Busca el registro de "idVentana" DENTRO de un launcher especifico;
   si no existe todavia, lo crea. Se debe llamar con
   rl->mutexVentanas YA tomado. */
static RegistroVentana *buscarOCrearRegistroVentana(RegistroLauncher *rl, int idVentana)
{
    int i;

    for (i = 0; i < rl->totalVentanas; i++)
    {
        if (rl->ventanas[i].idVentana == idVentana)
        {
            return &rl->ventanas[i];
        }
    }

    if (rl->totalVentanas == rl->capacidadVentanas)
    {
        int nuevaCapacidad = (rl->capacidadVentanas == 0) ? 8 : rl->capacidadVentanas * 2;
        RegistroVentana *nuevo = realloc(rl->ventanas, sizeof(RegistroVentana) * (size_t)nuevaCapacidad);

        if (!nuevo)
        {
            fprintf(stderr, "Sin memoria para registrar la ventana %d.\n", idVentana);
            return NULL;
        }

        rl->ventanas = nuevo;
        rl->capacidadVentanas = nuevaCapacidad;
    }

    rl->ventanas[rl->totalVentanas].idVentana = idVentana;
    documentoInicializar(&rl->ventanas[rl->totalVentanas].documento);
    rl->ventanas[rl->totalVentanas].clasificacionActual = DESCONOCIDO;
    rl->totalVentanas++;

    return &rl->ventanas[rl->totalVentanas - 1];
}

/* ================================================================
   Cola de oraciones pendientes (productor: hilos de conexion,
   consumidor: el hilo Loader). Es GLOBAL a todo el servidor -- P es
   un limite de recursos del "data center" completo, compartido por
   todos los launchers conectados; lo que se mantiene separado es el
   RESULTADO de la clasificacion (por eso cada oracion en la cola
   carga tambien su idLauncher, para saber a que contexto pertenece).
   ================================================================ */

typedef struct NodoOracion
{
    int idLauncher;
    int idVentana;
    char texto[TAMANO_BUFFER];
    struct NodoOracion *siguiente;
} NodoOracion;

static NodoOracion *colaFrente = NULL;
static NodoOracion *colaFinal = NULL;
static int colaCantidad = 0;

static pthread_mutex_t mutexCola = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condLoteListo = PTHREAD_COND_INITIALIZER;

static void encolarOracion(int idLauncher, int idVentana, const char *texto)
{
    NodoOracion *nodo = malloc(sizeof(NodoOracion));

    if (!nodo)
    {
        fprintf(stderr, "Sin memoria para encolar una oracion.\n");
        return;
    }

    nodo->idLauncher = idLauncher;
    nodo->idVentana = idVentana;
    strncpy(nodo->texto, texto, sizeof(nodo->texto) - 1);
    nodo->texto[sizeof(nodo->texto) - 1] = '\0';
    nodo->siguiente = NULL;

    pthread_mutex_lock(&mutexCola);

    if (colaFinal)
    {
        colaFinal->siguiente = nodo;
    }
    else
    {
        colaFrente = nodo;
    }
    colaFinal = nodo;
    colaCantidad++;

    if (colaCantidad >= P)
    {
        pthread_cond_signal(&condLoteListo);
    }

    pthread_mutex_unlock(&mutexCola);
}

static void sacarLoteDeCola(NodoOracion *lote[], int cantidad)
{
    int i;

    for (i = 0; i < cantidad; i++)
    {
        NodoOracion *nodo = colaFrente;

        colaFrente = nodo->siguiente;
        if (!colaFrente)
        {
            colaFinal = NULL;
        }
        colaCantidad--;

        lote[i] = nodo;
    }
}

/* ================================================================
   Decision de tipo de usuario -- POR LAUNCHER, asincronica: se
   reevalua cada vez que la clasificacion de una ventana DE ESE
   launcher cambia.
   ================================================================ */

static const char *NOMBRES_TIPO_USUARIO[4] =
{
    "Personal administrativo",
    "Personal tecnico",
    "Profesor",
    "Estudiante"
};

/* Tabla 2 de este parcial:
                              Correo  Articulo  Reporte
   Personal administrativo :   X                 X
   Personal tecnico        :   X
   Profesor                :   X       X
   Estudiante               :          X         X   */
static void evaluarTipoUsuario(RegistroLauncher *rl)
{
    int correo, articulo, reporte;
    int tipoDetectado = -1;
    int debeImprimir = 0;

    pthread_mutex_lock(&rl->mutexContadores);

    correo = rl->documentosCorreo;
    articulo = rl->documentosArticulo;
    reporte = rl->documentosReporte;

    if (correo > 0 && reporte > 0 && articulo == 0)
    {
        tipoDetectado = 0;
    }
    else if (correo > 0 && articulo == 0 && reporte == 0)
    {
        tipoDetectado = 1;
    }
    else if (correo > 0 && articulo > 0 && reporte == 0)
    {
        tipoDetectado = 2;
    }
    else if (articulo > 0 && reporte > 0 && correo == 0)
    {
        tipoDetectado = 3;
    }

    if (tipoDetectado != -1 && tipoDetectado != rl->ultimoTipoUsuarioImpreso)
    {
        rl->ultimoTipoUsuarioImpreso = tipoDetectado;
        debeImprimir = 1;
    }

    pthread_mutex_unlock(&rl->mutexContadores);

    if (debeImprimir)
    {
        printf("\n[ialearner] (asincronico) Launcher %d -> Tipo de usuario: %s\n\n",
               rl->idLauncher, NOMBRES_TIPO_USUARIO[tipoDetectado]);
        fflush(stdout);
    }
}

/* ================================================================
   El trabajo real de un hilo de deteccion: clasificar UNA oracion,
   dentro del contexto de SU launcher
   ================================================================ */

static void procesarOracion(int idLauncher, int idVentana, const char *texto)
{
    RegistroLauncher *rl = buscarOCrearLauncher(idLauncher);
    RegistroVentana *registro;
    int clasificacionAnterior;
    int clasificacionNueva;
    int cambioClasificacion;

    if (!rl)
    {
        return;
    }

    pthread_mutex_lock(&rl->mutexVentanas);

    registro = buscarOCrearRegistroVentana(rl, idVentana);

    if (!registro)
    {
        pthread_mutex_unlock(&rl->mutexVentanas);
        return;
    }

    {
        char copia[TAMANO_BUFFER];
        strncpy(copia, texto, sizeof(copia) - 1);
        copia[sizeof(copia) - 1] = '\0';
        documentoAgregarLinea(&registro->documento, copia);
    }

    clasificacionAnterior = registro->clasificacionActual;
    clasificacionNueva = documentoClasificar(&registro->documento);
    cambioClasificacion = (clasificacionNueva != clasificacionAnterior);

    if (cambioClasificacion)
    {
        registro->clasificacionActual = clasificacionNueva;
    }

    pthread_mutex_unlock(&rl->mutexVentanas);

    {
        char copia[TAMANO_BUFFER];
        strncpy(copia, texto, sizeof(copia) - 1);
        copia[sizeof(copia) - 1] = '\0';

        pthread_mutex_lock(&rl->mutexContadores);
        documentoAgregarLinea(&rl->matrizGlobal, copia);
        pthread_mutex_unlock(&rl->mutexContadores);
    }

    if (cambioClasificacion)
    {
        pthread_mutex_lock(&rl->mutexContadores);

        if (clasificacionAnterior == EMAIL) rl->documentosCorreo--;
        else if (clasificacionAnterior == ARTICULO) rl->documentosArticulo--;
        else if (clasificacionAnterior == REPORTE) rl->documentosReporte--;

        if (clasificacionNueva == EMAIL) rl->documentosCorreo++;
        else if (clasificacionNueva == ARTICULO) rl->documentosArticulo++;
        else if (clasificacionNueva == REPORTE) rl->documentosReporte++;

        pthread_mutex_unlock(&rl->mutexContadores);

        if (clasificacionNueva != DESCONOCIDO)
        {
            evaluarTipoUsuario(rl);
        }
    }
}

/* ================================================================
   Pool de P hilos de deteccion + hilo Loader (sin cambios respecto
   a la version anterior: P es un recurso del servidor completo,
   compartido por todos los launchers)
   ================================================================ */

static NodoOracion **loteActual;
static long generacionLote = 0;
static int trabajadoresPendientes = 0;

static pthread_mutex_t mutexLote = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condLoteAsignado = PTHREAD_COND_INITIALIZER;
static pthread_cond_t condLoteTerminado = PTHREAD_COND_INITIALIZER;

typedef struct
{
    int indice;
} ArgsHiloDeteccion;

static void *hiloDeteccion(void *arg)
{
    ArgsHiloDeteccion *args = (ArgsHiloDeteccion *)arg;
    int miIndice = args->indice;
    long miUltimaGeneracion = 0;

    free(args);

    while (1)
    {
        NodoOracion *miNodo;

        pthread_mutex_lock(&mutexLote);
        while (generacionLote == miUltimaGeneracion)
        {
            pthread_cond_wait(&condLoteAsignado, &mutexLote);
        }
        miUltimaGeneracion = generacionLote;
        miNodo = loteActual[miIndice];
        pthread_mutex_unlock(&mutexLote);

        printf("[deteccion %d] procesando oracion (launcher %d, ventana %d), lote %ld\n",
               miIndice, miNodo->idLauncher, miNodo->idVentana, miUltimaGeneracion);
        fflush(stdout);

        /* Trabajo real, EN PARALELO con los otros P-1 hilos: cada uno
           toca SOLO su propio "miNodo" en este punto, sin mutex. */
        procesarOracion(miNodo->idLauncher, miNodo->idVentana, miNodo->texto);

        printf("[deteccion %d] termino (launcher %d, ventana %d)\n",
               miIndice, miNodo->idLauncher, miNodo->idVentana);
        fflush(stdout);

        free(miNodo);

        pthread_mutex_lock(&mutexLote);
        trabajadoresPendientes--;
        if (trabajadoresPendientes == 0)
        {
            pthread_cond_signal(&condLoteTerminado);
        }
        pthread_mutex_unlock(&mutexLote);
    }

    return NULL;
}

static void *hiloLoader(void *arg)
{
    NodoOracion **nodosLote = malloc(sizeof(NodoOracion *) * (size_t)P);
    int i;

    (void)arg;

    if (!nodosLote)
    {
        fprintf(stderr, "Sin memoria para el Loader.\n");
        return NULL;
    }

    while (1)
    {
        pthread_mutex_lock(&mutexCola);
        while (colaCantidad < P)
        {
            pthread_cond_wait(&condLoteListo, &mutexCola);
        }
        sacarLoteDeCola(nodosLote, P);
        pthread_mutex_unlock(&mutexCola);

        pthread_mutex_lock(&mutexLote);
        for (i = 0; i < P; i++)
        {
            loteActual[i] = nodosLote[i];
        }
        trabajadoresPendientes = P;
        generacionLote++;
        pthread_cond_broadcast(&condLoteAsignado);

        while (trabajadoresPendientes > 0)
        {
            pthread_cond_wait(&condLoteTerminado, &mutexLote);
        }
        pthread_mutex_unlock(&mutexLote);
    }

    free(nodosLote);
    return NULL;
}

/* ================================================================
   Hilos de conexion: uno por ventana, solo leen la red y encolan
   ================================================================ */

typedef struct
{
    int socketCliente;
} ArgsConexion;

static int siguienteIdVentana = 1;
static pthread_mutex_t mutexIdVentana = PTHREAD_MUTEX_INITIALIZER;

static int generarIdVentana(void)
{
    int id;

    pthread_mutex_lock(&mutexIdVentana);
    id = siguienteIdVentana++;
    pthread_mutex_unlock(&mutexIdVentana);

    return id;
}

/* Lee una linea completa desde el socket "cliente" (hasta un '\n',
   que NO se incluye en el resultado). Devuelve 0 si logro leer una
   linea completa, o -1 si la conexion se cerro antes de completarla. */
static int leerLineaSocket(int cliente, char *buffer, size_t tam)
{
    size_t indice = 0;
    char c;
    ssize_t leidos;

    while ((leidos = recv(cliente, &c, 1, 0)) > 0)
    {
        if (c == '\n')
        {
            buffer[indice] = '\0';
            return 0;
        }
        if (indice < tam - 1)
        {
            buffer[indice++] = c;
        }
    }

    return -1;
}

static int clientesConectados = 0;
static pthread_mutex_t mutexClientesConectados = PTHREAD_MUTEX_INITIALIZER;

void *atenderCliente(void *arg)
{
    ArgsConexion *args = (ArgsConexion *)arg;
    int cliente = args->socketCliente;
    int idVentana;
    int idLauncher;
    char primeraLinea[TAMANO_BUFFER];
    int primeraLineaEsOracion = 0;
    char c;
    char linea[TAMANO_BUFFER];
    int indice = 0;
    ssize_t leidos;

    free(args);

    /* Se lee la primera linea que manda el cliente: debe ser un
       handshake "VENTANA <id> LAUNCHER <id>" (el "verificador" del
       paquete de datos), enviado por window.c justo despues de
       conectar. Esto es lo que le permite a ialearner saber tanto de
       que VENTANA viene una oracion como de que LAUNCHER (que
       "computadora"/usuario) es, para mantener sus contextos
       separados como se aclaro en clase.

       Si la primera linea NO tiene ese formato (por ejemplo, alguien
       corrio "window" a mano sin pasar por el launcher), esa linea
       NO se descarta: se trata como la primera oracion real, y se le
       asigna un id de respaldo tanto de ventana como de launcher
       (launcher 0 = "sin identificar"). */
    if (leerLineaSocket(cliente, primeraLinea, sizeof(primeraLinea)) != 0)
    {
        close(cliente);
        return NULL;
    }

    {
        int idVentanaRecibido = 0;
        int idLauncherRecibido = 0;

        if (sscanf(primeraLinea, "VENTANA %d LAUNCHER %d", &idVentanaRecibido, &idLauncherRecibido) == 2 &&
            idVentanaRecibido > 0 && idLauncherRecibido > 0)
        {
            idVentana = idVentanaRecibido;
            idLauncher = idLauncherRecibido;
        }
        else
        {
            idVentana = generarIdVentana();
            idLauncher = 0;
            primeraLineaEsOracion = 1;
        }
    }

    printf("Nuevo hilo de conexion (launcher %d, ventana %d).\n", idLauncher, idVentana);
    fflush(stdout);

    if (primeraLineaEsOracion && primeraLinea[0] != '\0')
    {
        printf("[ialearner] [launcher %d][ventana %d] oracion recibida: %s\n",
               idLauncher, idVentana, primeraLinea);
        fflush(stdout);
        encolarOracion(idLauncher, idVentana, primeraLinea);
    }

    while ((leidos = recv(cliente, &c, 1, 0)) > 0)
    {
        if (c == '\n')
        {
            linea[indice] = '\0';
            printf("[ialearner] [launcher %d][ventana %d] oracion recibida: %s\n",
                   idLauncher, idVentana, linea);
            fflush(stdout);

            encolarOracion(idLauncher, idVentana, linea);

            indice = 0;
        }
        else if (indice < (int)sizeof(linea) - 1)
        {
            linea[indice++] = c;
        }
    }

    if (leidos < 0)
    {
        perror("recv");
    }

    if (indice > 0)
    {
        linea[indice] = '\0';
        printf("[ialearner] [launcher %d][ventana %d] oracion final (sin Enter): %s\n",
               idLauncher, idVentana, linea);
        fflush(stdout);
        encolarOracion(idLauncher, idVentana, linea);
    }

    printf("[launcher %d][ventana %d] Cliente desconectado.\n", idLauncher, idVentana);
    fflush(stdout);

    pthread_mutex_lock(&mutexClientesConectados);
    clientesConectados--;
    if (clientesConectados == 0)
    {
        mostrarResumenFinal();
    }
    pthread_mutex_unlock(&mutexClientesConectados);

    close(cliente);

    return NULL;
}

/* ================================================================
   Resumen (snapshot manual, separado por launcher)
   ================================================================ */

void mostrarResumenFinal(void)
{
    int i;

    pthread_mutex_lock(&mutexLaunchers);

    printf("\n=============================\n");
    printf("RESUMEN ACUMULADO (por launcher / computadora)\n");

    if (totalLaunchers == 0)
    {
        printf("Todavia no se ha conectado ningun launcher.\n");
    }

    for (i = 0; i < totalLaunchers; i++)
    {
        RegistroLauncher *rl = launchers[i];
        int total;

        pthread_mutex_lock(&rl->mutexContadores);

        printf("\n--- Launcher %d ---\n", rl->idLauncher);
        printf("Correo electronico : %d\n", rl->documentosCorreo);
        printf("Articulo cientifico: %d\n", rl->documentosArticulo);
        printf("Reporte            : %d\n", rl->documentosReporte);
        printf("Matriz global de este launcher:\n");
        documentoImprimirResumen(&rl->matrizGlobal);

        total = rl->documentosCorreo + rl->documentosArticulo + rl->documentosReporte;
        if (total == 0)
        {
            printf("(sin documentos clasificados todavia)\n");
        }

        pthread_mutex_unlock(&rl->mutexContadores);
    }

    printf("=============================\n\n");
    fflush(stdout);

    pthread_mutex_unlock(&mutexLaunchers);
}

/* ================================================================
   Arranque del servidor
   ================================================================ */

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
    const char *carpetaDiccionariosArg = NULL;
    char carpetaResuelta[PATH_MAX];
    const char *carpetaDiccionarios;
    int servidor;
    int i;
    int posicionales = 0;
    pthread_t *hilosDeteccion;
    pthread_t hiloLoaderTid;

    P = 0;

    for (i = 1; i < argc; i++)
    {
        if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--hilos") == 0) && i + 1 < argc)
        {
            P = atoi(argv[i + 1]);
            i++;
        }
        else if (posicionales == 0)
        {
            puerto = argv[i];
            posicionales++;
        }
        else if (posicionales == 1)
        {
            carpetaDiccionariosArg = argv[i];
            posicionales++;
        }
    }

    cantidadCPUs = detectarCantidadCPUs();

    if (P <= 0)
    {
        P = cantidadCPUs;
    }

    if (obtenerCarpetaDiccionariosPorDefecto(carpetaResuelta, sizeof(carpetaResuelta)) == 0)
    {
        carpetaDiccionarios = carpetaResuelta;
    }
    else
    {
        carpetaDiccionarios = CARPETA_DICCIONARIOS_DEFECTO;
    }

    if (carpetaDiccionariosArg != NULL)
    {
        carpetaDiccionarios = carpetaDiccionariosArg;
    }

    if (cargarDiccionarios(carpetaDiccionarios) != 0)
    {
        fprintf(stderr, "No se pudieron cargar los diccionarios desde '%s'.\n", carpetaDiccionarios);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    servidor = crearSocketEscucha(puerto);

    if (servidor < 0)
    {
        liberarDiccionarios();
        return EXIT_FAILURE;
    }

    printf("CPUs detectados en esta maquina: %d\n", cantidadCPUs);
    printf("Hilos de deteccion (P): %d\n", P);
    printf("IA Learner escuchando en el puerto %s.\n", puerto);
    fflush(stdout);

    loteActual = malloc(sizeof(NodoOracion *) * (size_t)P);
    hilosDeteccion = malloc(sizeof(pthread_t) * (size_t)P);

    if (!loteActual || !hilosDeteccion)
    {
        fprintf(stderr, "Sin memoria para crear el pool de hilos de deteccion.\n");
        liberarDiccionarios();
        return EXIT_FAILURE;
    }

    for (i = 0; i < P; i++)
    {
        ArgsHiloDeteccion *args = malloc(sizeof(ArgsHiloDeteccion));

        if (!args)
        {
            fprintf(stderr, "Sin memoria para el hilo de deteccion %d.\n", i);
            continue;
        }

        args->indice = i;

        if (pthread_create(&hilosDeteccion[i], NULL, hiloDeteccion, args) != 0)
        {
            perror("pthread_create (deteccion)");
            free(args);
            continue;
        }

        asignarCPU(hilosDeteccion[i], i);
        pthread_detach(hilosDeteccion[i]);
    }

    if (pthread_create(&hiloLoaderTid, NULL, hiloLoader, NULL) != 0)
    {
        perror("pthread_create (Loader)");
        liberarDiccionarios();
        return EXIT_FAILURE;
    }
    pthread_detach(hiloLoaderTid);

    while (1)
    {
        ArgsConexion *args = malloc(sizeof(ArgsConexion));
        pthread_t hilo;

        if (!args)
        {
            fprintf(stderr, "Sin memoria para atender una nueva conexion.\n");
            continue;
        }

        args->socketCliente = accept(servidor, NULL, NULL);

        if (args->socketCliente < 0)
        {
            perror("accept");
            free(args);
            continue;
        }

        printf("Nueva conexion aceptada, esperando identificacion...\n");
        fflush(stdout);

        pthread_mutex_lock(&mutexClientesConectados);
        clientesConectados++;
        pthread_mutex_unlock(&mutexClientesConectados);

        if (pthread_create(&hilo, NULL, atenderCliente, args) != 0)
        {
            perror("pthread_create (conexion)");

            pthread_mutex_lock(&mutexClientesConectados);
            clientesConectados--;
            pthread_mutex_unlock(&mutexClientesConectados);

            close(args->socketCliente);
            free(args);
            continue;
        }

        pthread_detach(hilo);
    }

    close(servidor);
    liberarDiccionarios();

    return 0;
}
