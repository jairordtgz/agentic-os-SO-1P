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

int documentosCorreo = 0;
int documentosArticulo = 0;
int documentosReporte = 0;
int clientesConectados = 0;

DocumentoVentana matrizGlobal;

/* Mutex "general": protege documentosCorreo/Articulo/Reporte,
   clientesConectados, matrizGlobal y el ultimo tipo de usuario
   impreso. Es intencional reusar un solo mutex para estas variables:
   siempre se leen/escriben juntas y las secciones son muy cortas, asi
   que separar en mas mutexes aqui no reduciria contencion real, solo
   agregaria complejidad. */
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/* ================================================================
   Deteccion del entorno: CPUs y tamaño del pool de hilos (P)
   ================================================================ */

static int cantidadCPUs;
static int P;  /* cantidad de hilos de deteccion, parametro -p / --hilos */

/* Detecta cuantos nucleos de CPU tiene ESTA maquina en tiempo de
   ejecucion. Nunca se recibe como parametro -- se pide asi en el
   enunciado, porque cada "computador" (simulado con un puerto
   distinto) puede tener una cantidad de CPUs distinta, y el programa
   debe adaptarse solo a la maquina donde corre. */
static int detectarCantidadCPUs(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    if (n < 1)
    {
        return 1;  /* respaldo defensivo, casi nunca deberia pasar */
    }

    return (int)n;
}

/* Fija el hilo "hilo" a un CPU especifico, repartiendo los P hilos de
   deteccion entre los CPUs disponibles en "round robin" (hilo 0 -> CPU 0,
   hilo 1 -> CPU 1, ..., y si hay mas hilos que CPUs, se reparte en
   vueltas). Esto es el balanceo de carga del requerimiento 7a: en vez
   de dejar que el planificador del sistema operativo decida sin
   ninguna guia, se distribuyen explicitamente los hilos que SI hacen
   trabajo pesado (el algoritmo de deteccion) entre todos los nucleos. */
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
   Cola de oraciones pendientes (productor: hilos de conexion,
   consumidor: el hilo Loader)
   ================================================================ */

typedef struct NodoOracion
{
    int idVentana;
    char texto[TAMANO_BUFFER];
    struct NodoOracion *siguiente;
} NodoOracion;

static NodoOracion *colaFrente = NULL;
static NodoOracion *colaFinal = NULL;
static int colaCantidad = 0;

static pthread_mutex_t mutexCola = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condLoteListo = PTHREAD_COND_INITIALIZER;  /* avisa al Loader que ya hay >= P */

/* Encola una oracion recien completada. La llaman los hilos de
   conexion (uno por ventana) cada vez que reciben un '\n'. */
static void encolarOracion(int idVentana, const char *texto)
{
    NodoOracion *nodo = malloc(sizeof(NodoOracion));

    if (!nodo)
    {
        fprintf(stderr, "Sin memoria para encolar una oracion.\n");
        return;
    }

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

/* Saca EXACTAMENTE "cantidad" nodos del frente de la cola y los deja
   en "lote[]". Se debe llamar con mutexCola YA tomado, y solo despues
   de confirmar que colaCantidad >= cantidad. */
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
   Registro de documentos por ventana (compartido: oraciones de la
   MISMA ventana pueden ser procesadas por hilos de deteccion
   DISTINTOS en lotes distintos, asi que el vector acumulado de cada
   ventana necesita su propia proteccion).
   ================================================================ */

typedef struct
{
    int idVentana;
    DocumentoVentana documento;
    int clasificacionActual;  /* EMAIL / ARTICULO / REPORTE / DESCONOCIDO */
} RegistroVentana;

static RegistroVentana *registros = NULL;
static int totalRegistros = 0;
static int capacidadRegistros = 0;
static pthread_mutex_t mutexRegistros = PTHREAD_MUTEX_INITIALIZER;

/* Busca el registro de "idVentana"; si no existe todavia (es la
   primera oracion que llega de esa ventana), lo crea. Se debe llamar
   con mutexRegistros YA tomado. */
static RegistroVentana *buscarOCrearRegistro(int idVentana)
{
    int i;

    for (i = 0; i < totalRegistros; i++)
    {
        if (registros[i].idVentana == idVentana)
        {
            return &registros[i];
        }
    }

    if (totalRegistros == capacidadRegistros)
    {
        int nuevaCapacidad = (capacidadRegistros == 0) ? 8 : capacidadRegistros * 2;
        RegistroVentana *nuevo = realloc(registros, sizeof(RegistroVentana) * (size_t)nuevaCapacidad);

        if (!nuevo)
        {
            fprintf(stderr, "Sin memoria para registrar la ventana %d.\n", idVentana);
            return NULL;
        }

        registros = nuevo;
        capacidadRegistros = nuevaCapacidad;
    }

    registros[totalRegistros].idVentana = idVentana;
    documentoInicializar(&registros[totalRegistros].documento);
    registros[totalRegistros].clasificacionActual = DESCONOCIDO;
    totalRegistros++;

    return &registros[totalRegistros - 1];
}

/* ================================================================
   Decision de tipo de usuario (asincronica: se reevalua cada vez que
   la clasificacion de alguna ventana cambia)
   ================================================================ */

static int ultimoTipoUsuarioImpreso = -1;  /* -1 = "nada impreso todavia" */

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
static void evaluarTipoUsuario(void)
{
    int correo, articulo, reporte;
    int tipoDetectado = -1;
    int debeImprimir = 0;

    pthread_mutex_lock(&mutex);

    correo = documentosCorreo;
    articulo = documentosArticulo;
    reporte = documentosReporte;

    if (correo > 0 && reporte > 0 && articulo == 0)
    {
        tipoDetectado = 0;  /* Personal administrativo */
    }
    else if (correo > 0 && articulo == 0 && reporte == 0)
    {
        tipoDetectado = 1;  /* Personal tecnico */
    }
    else if (correo > 0 && articulo > 0 && reporte == 0)
    {
        tipoDetectado = 2;  /* Profesor */
    }
    else if (articulo > 0 && reporte > 0 && correo == 0)
    {
        tipoDetectado = 3;  /* Estudiante */
    }

    if (tipoDetectado != -1 && tipoDetectado != ultimoTipoUsuarioImpreso)
    {
        ultimoTipoUsuarioImpreso = tipoDetectado;
        debeImprimir = 1;
    }

    pthread_mutex_unlock(&mutex);

    /* El printf se hace FUERA del mutex a proposito: imprimir puede
       bloquear en I/O, y la seccion critica debe ser lo mas corta
       posible. */
    if (debeImprimir)
    {
        printf("\n[ialearner] (asincronico) Tipo de usuario detectado: %s\n\n",
               NOMBRES_TIPO_USUARIO[tipoDetectado]);
        fflush(stdout);
    }
}

/* ================================================================
   El trabajo real de un hilo de deteccion: clasificar UNA oracion
   ================================================================ */

static void procesarOracion(int idVentana, const char *texto)
{
    RegistroVentana *registro;
    int clasificacionAnterior;
    int clasificacionNueva;
    int cambioClasificacion;

    pthread_mutex_lock(&mutexRegistros);

    registro = buscarOCrearRegistro(idVentana);

    if (!registro)
    {
        pthread_mutex_unlock(&mutexRegistros);
        return;
    }

    /* char[] no-const porque documentoAgregarLinea() lo requiere asi,
       aunque no lo modifica; se copia a un buffer local para no tocar
       "texto" (que es const desde el llamador). */
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

    pthread_mutex_unlock(&mutexRegistros);

    /* Actualiza tambien la matriz global (todas las ventanas
       combinadas), protegida con el mutex general. */
    {
        char copia[TAMANO_BUFFER];
        strncpy(copia, texto, sizeof(copia) - 1);
        copia[sizeof(copia) - 1] = '\0';

        pthread_mutex_lock(&mutex);
        documentoAgregarLinea(&matrizGlobal, copia);
        pthread_mutex_unlock(&mutex);
    }

    if (cambioClasificacion)
    {
        pthread_mutex_lock(&mutex);

        if (clasificacionAnterior == EMAIL) documentosCorreo--;
        else if (clasificacionAnterior == ARTICULO) documentosArticulo--;
        else if (clasificacionAnterior == REPORTE) documentosReporte--;

        if (clasificacionNueva == EMAIL) documentosCorreo++;
        else if (clasificacionNueva == ARTICULO) documentosArticulo++;
        else if (clasificacionNueva == REPORTE) documentosReporte++;

        pthread_mutex_unlock(&mutex);

        /* "Apenas se detecte el tipo de documento... se evalua si es
           posible determinar el tipo de usuario" (requerimiento 7d). */
        if (clasificacionNueva != DESCONOCIDO)
        {
            evaluarTipoUsuario();
        }
    }
}

/* ================================================================
   Pool de P hilos de deteccion + hilo Loader
   ================================================================ */

static NodoOracion **loteActual;       /* arreglo de P punteros: la oracion asignada a cada hilo en el lote vigente */
static long generacionLote = 0;        /* se incrementa cada vez que el Loader arma un lote nuevo (evita que un
                                           hilo procese el mismo lote dos veces -- ver hiloDeteccion) */
static int trabajadoresPendientes = 0;

static pthread_mutex_t mutexLote = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condLoteAsignado = PTHREAD_COND_INITIALIZER;   /* Loader -> hilos: "su oracion ya esta lista" */
static pthread_cond_t condLoteTerminado = PTHREAD_COND_INITIALIZER;  /* hilos -> Loader: "ya termine mi parte" */

typedef struct
{
    int indice;
} ArgsHiloDeteccion;

/* Cada uno de los P hilos de deteccion se queda BLOQUEADO (sin gastar
   CPU) hasta que el Loader arma un lote nuevo. "miUltimaGeneracion"
   es la clave para que un hilo no vuelva a procesar el mismo lote dos
   veces: solo avanza cuando "generacionLote" es MAYOR que la ultima
   generacion que el ya proceso -- no basta con una bandera booleana,
   porque un hilo que termina temprano podria "adelantarse" y leer el
   mismo lote otra vez antes de que el Loader arme uno nuevo. */
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

	printf("[deteccion %d] procesando oracion de ventana %d (lote generacion %ld)\n",
               miIndice, miNodo->idVentana, miUltimaGeneracion);
        fflush(stdout);

        /* Trabajo real, EN PARALELO con los otros P-1 hilos: cada uno
           toca SOLO su propio "miNodo" en este punto, sin mutex. */
        procesarOracion(miNodo->idVentana, miNodo->texto);

	printf("[deteccion %d] termino de procesar la oracion de ventana %d\n",
               miIndice, miNodo->idVentana);
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

/* El Loader: espera a que se acumulen P oraciones en la cola, arma un
   lote, despierta a los P hilos de deteccion A LA VEZ, y espera a que
   TODOS terminen antes de armar el siguiente lote (requerimientos
   7c y 7d). */
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

    free(nodosLote);  /* nunca se alcanza en la practica: el bucle es infinito */
    return NULL;
}

/* ================================================================
   Hilos de conexion: uno por ventana, solo leen la red y encolan
   ================================================================ */

typedef struct
{
    int socketCliente;
    int idVentana;
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

void *atenderCliente(void *arg)
{
    ArgsConexion *args = (ArgsConexion *)arg;
    int cliente = args->socketCliente;
    int idVentana = args->idVentana;
    char c;
    char linea[TAMANO_BUFFER];
    int indice = 0;
    ssize_t leidos;

    free(args);

    printf("Nuevo hilo de conexion (ventana %d).\n", idVentana);
    fflush(stdout);

    while ((leidos = recv(cliente, &c, 1, 0)) > 0)
    {
        if (c == '\n')
        {
            linea[indice] = '\0';
            printf("[ialearner] [ventana %d] oracion recibida: %s\n", idVentana, linea);
            fflush(stdout);

            /* En vez de clasificar aqui mismo, se encola para que la
               procese el pool de P hilos de deteccion -- esto es lo
               que desacopla "leer la red" de "correr el algoritmo",
               permitiendo limitar el algoritmo a P hilos sin importar
               cuantas ventanas esten conectadas. */
            encolarOracion(idVentana, linea);

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
        printf("[ialearner] [ventana %d] oracion final (sin Enter): %s\n", idVentana, linea);
        fflush(stdout);
        encolarOracion(idVentana, linea);
    }

    printf("[ventana %d] Cliente desconectado.\n", idVentana);
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

/* ================================================================
   Resumen (snapshot manual del estado acumulado hasta el momento)
   ================================================================ */

void mostrarResumenFinal(void)
{
    int total = documentosCorreo + documentosArticulo + documentosReporte;

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
    }

    printf("=============================\n\n");
    fflush(stdout);
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

    /* ---- Analisis de argumentos ----
       -p N / --hilos N  : cantidad de hilos de deteccion (P). Puede ir
                            en cualquier posicion.
       El resto de los argumentos, EN ORDEN, son: puerto, carpeta de
       diccionarios (ambos opcionales, con valores por defecto). */
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

    /* La cantidad de CPUs NUNCA se recibe como parametro: siempre se
       detecta en tiempo de ejecucion, porque cada "computador"
       (simulado con un puerto distinto) puede correr en una maquina
       distinta con una cantidad de nucleos distinta. */
    cantidadCPUs = detectarCantidadCPUs();

    if (P <= 0)
    {
        /* Si no se paso -p (o se paso un valor invalido), se usa un
           valor por defecto razonable basado en los CPUs detectados,
           en vez de fallar o de usar un numero fijo arbitrario. */
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

    documentoInicializar(&matrizGlobal);

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

    /* ---- Arrancar el pool de P hilos de deteccion + el Loader ---- */
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

    /* ---- Bucle principal: aceptar conexiones de ventanas ---- */
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

        args->idVentana = generarIdVentana();

        printf("Nueva conexion (ventana %d).\n", args->idVentana);
        fflush(stdout);

        pthread_mutex_lock(&mutex);
        clientesConectados++;
        pthread_mutex_unlock(&mutex);

        if (pthread_create(&hilo, NULL, atenderCliente, args) != 0)
        {
            perror("pthread_create (conexion)");

            pthread_mutex_lock(&mutex);
            clientesConectados--;
            pthread_mutex_unlock(&mutex);

            close(args->socketCliente);
            free(args);
            continue;
        }

        pthread_detach(hilo);
    }

    close(servidor);
    documentoLiberar(&matrizGlobal);
    liberarDiccionarios();

    return 0;
}
