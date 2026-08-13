#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "classifier.h"

/* Los nombres de las 3 categorias son fijos: los define el propio
   enunciado del proyecto (Correo, Articulo, Reporte), no algo que el
   usuario deba poder cambiar. Por eso van aqui como constantes, y NO
   se leen de los archivos .txt -- lo unico que viene de los .txt son
   las PALABRAS de cada categoria, que es lo que el profesor pidio
   sacar del codigo fuente. */
static const char *NOMBRES_CATEGORIAS[NUM_DICCIONARIOS] =
{
    "Correo electronico",
    "Articulo cientifico",
    "Reporte"
};

/* ---- Tabla hash por diccionario ----
   Cada diccionario tiene su propia tabla hash: en vez de recorrer
   con un for las palabras una por una (busqueda O(n)) para saber si
   una palabra pertenece al diccionario, se calcula un numero (hash)
   a partir del texto de la palabra y se va directo al "cajon" donde
   esa palabra deberia estar (busqueda O(1) en promedio). Como puede
   haber dos palabras distintas que caigan en el mismo cajon (una
   "colision"), cada cajon guarda una lista enlazada de las palabras
   que cayeron ahi -- por eso cada nodo tiene un "siguiente".

   El numero de cajones (TAMANO_TABLA_HASH) es fijo, pero eso NO limita
   cuantas palabras puede tener un diccionario: solo afecta que tan
   cortas quedan las listas de cada cajon. Con un numero generoso de
   cajones, incluso diccionarios de varios cientos de palabras siguen
   teniendo busquedas rapidas. */
#define TAMANO_TABLA_HASH 4099
#define LONGITUD_MAX_PALABRA 63

typedef struct NodoHash
{
    char palabra[LONGITUD_MAX_PALABRA + 1];
    int indice;                    /* posicion de esta palabra dentro del vector de frecuencias */
    struct NodoHash *siguiente;    /* siguiente nodo en este mismo cajon, si hay colision */
} NodoHash;

typedef struct
{
    int cantidadPalabras;          /* SIN tope: crece con lo que traiga el archivo */
    NodoHash *tabla[TAMANO_TABLA_HASH];
} Diccionario;

static Diccionario diccionarios[NUM_DICCIONARIOS];

static void convertirMinusculas(char texto[])
{
    int i;

    for (i = 0; texto[i] != '\0'; i++)
    {
        texto[i] = (char)tolower((unsigned char)texto[i]);
    }
}

static int esSeparador(char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t' ||
           c == ',' || c == '.' || c == ';' || c == ':';
}

/* Funcion hash tipo "djb2": recorre la palabra caracter por caracter
   y va combinando esos valores en un solo numero grande, de forma
   que palabras distintas casi siempre terminen en cajones distintos.
   El "% TAMANO_TABLA_HASH" al final asegura que el resultado siempre
   caiga dentro del rango valido de la tabla. */
static unsigned int funcionHash(const char *palabra)
{
    unsigned int hash = 5381;
    int c;

    while ((c = (unsigned char)*palabra++) != '\0')
    {
        hash = ((hash << 5) + hash) + (unsigned int)c;  /* hash * 33 + c */
    }

    return hash % TAMANO_TABLA_HASH;
}

/* Inserta "palabra" en la tabla hash del diccionario, en la posicion
   de vector "indice". Se usa SOLO durante la carga inicial, antes de
   crear ningun hilo -- por eso no necesita mutex: cuando los hilos
   empiezan a correr, esta tabla ya no se vuelve a escribir, solo se
   lee (y leer al mismo tiempo desde varios hilos, sin que nadie
   escriba, es seguro). */
static void hashInsertar(Diccionario *dic, const char *palabra, int indice)
{
    unsigned int posicion = funcionHash(palabra);
    NodoHash *nodo = malloc(sizeof(NodoHash));

    if (!nodo)
    {
        fprintf(stderr, "Sin memoria para cargar el diccionario.\n");
        return;
    }

    strncpy(nodo->palabra, palabra, LONGITUD_MAX_PALABRA);
    nodo->palabra[LONGITUD_MAX_PALABRA] = '\0';
    nodo->indice = indice;

    /* Se inserta al inicio de la lista de ese cajon: O(1). */
    nodo->siguiente = dic->tabla[posicion];
    dic->tabla[posicion] = nodo;
}

/* Busca "palabra" en el diccionario usando la tabla hash. Devuelve
   la posicion dentro del vector de frecuencias si la encuentra, o
   -1 si no pertenece a este diccionario. En promedio es O(1). */
static int buscarPalabra(const Diccionario *dic, const char palabra[])
{
    unsigned int posicion = funcionHash(palabra);
    NodoHash *nodo = dic->tabla[posicion];

    while (nodo != NULL)
    {
        if (strcmp(nodo->palabra, palabra) == 0)
        {
            return nodo->indice;
        }

        nodo = nodo->siguiente;
    }

    return -1;
}

/* Carga UN diccionario desde un archivo de texto: una palabra por
   linea, sin ningun encabezado ni nombre de categoria dentro del
   archivo (los nombres de categoria son fijos, ver NOMBRES_CATEGORIAS
   arriba). No hay tope de cuantas lineas puede tener el archivo. */
static int cargarDiccionario(const char *ruta, Diccionario *dic)
{
    FILE *archivo = fopen(ruta, "r");
    char linea[128];

    if (!archivo)
    {
        fprintf(stderr, "No se pudo abrir el diccionario: %s\n", ruta);
        return -1;
    }

    dic->cantidadPalabras = 0;

    while (fgets(linea, sizeof(linea), archivo))
    {
        linea[strcspn(linea, "\n")] = '\0';

        if (linea[0] == '\0')
        {
            continue;  /* ignora lineas en blanco en el archivo */
        }

        convertirMinusculas(linea);
        hashInsertar(dic, linea, dic->cantidadPalabras);
        dic->cantidadPalabras++;
    }

    fclose(archivo);

    if (dic->cantidadPalabras == 0)
    {
        fprintf(stderr, "El diccionario no tiene ninguna palabra: %s\n", ruta);
        return -1;
    }

    return 0;
}

int cargarDiccionarios(const char *carpeta)
{
    char rutaCorreo[256];
    char rutaArticulo[256];
    char rutaReporte[256];

    snprintf(rutaCorreo, sizeof(rutaCorreo), "%s/correo.txt", carpeta);
    snprintf(rutaArticulo, sizeof(rutaArticulo), "%s/articulo.txt", carpeta);
    snprintf(rutaReporte, sizeof(rutaReporte), "%s/reporte.txt", carpeta);

    if (cargarDiccionario(rutaCorreo, &diccionarios[EMAIL]) != 0)
    {
        return -1;
    }
    if (cargarDiccionario(rutaArticulo, &diccionarios[ARTICULO]) != 0)
    {
        return -1;
    }
    if (cargarDiccionario(rutaReporte, &diccionarios[REPORTE]) != 0)
    {
        return -1;
    }

    printf("Diccionarios cargados: %s (%d palabras), %s (%d palabras), %s (%d palabras)\n",
           NOMBRES_CATEGORIAS[EMAIL], diccionarios[EMAIL].cantidadPalabras,
           NOMBRES_CATEGORIAS[ARTICULO], diccionarios[ARTICULO].cantidadPalabras,
           NOMBRES_CATEGORIAS[REPORTE], diccionarios[REPORTE].cantidadPalabras);

    return 0;
}

void liberarDiccionarios(void)
{
    int i, j;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        for (j = 0; j < TAMANO_TABLA_HASH; j++)
        {
            NodoHash *nodo = diccionarios[i].tabla[j];

            while (nodo != NULL)
            {
                NodoHash *siguiente = nodo->siguiente;
                free(nodo);
                nodo = siguiente;
            }

            diccionarios[i].tabla[j] = NULL;
        }
    }
}

/* Suma, dentro de "vector", las frecuencias de las palabras de
   "texto" que aparecen en el diccionario "dic". El vector NO se
   reinicia aqui: se le va sumando lo que traiga, para poder acumular
   varias lineas seguidas de la misma ventana. */
static void acumularVector(const char texto[], const Diccionario *dic, int vector[])
{
    char copia[1024];
    char palabra[100];
    int indice = 0;
    int longitud;
    int i;

    strncpy(copia, texto, sizeof(copia) - 1);
    copia[sizeof(copia) - 1] = '\0';

    convertirMinusculas(copia);
    longitud = (int)strlen(copia);

    for (i = 0; i <= longitud; i++)
    {
        char c = copia[i];

        if (esSeparador(c) || c == '\0')
        {
            if (indice > 0)
            {
                int posicion;

                palabra[indice] = '\0';
                posicion = buscarPalabra(dic, palabra);

                if (posicion != -1)
                {
                    vector[posicion]++;
                }

                indice = 0;
            }
        }
        else if (indice < (int)sizeof(palabra) - 1)
        {
            palabra[indice++] = c;
        }
    }
}

/* Reserva un vector de frecuencias por cada diccionario, del tamaño
   EXACTO que tenga ese diccionario (diccionarios[i].cantidadPalabras).
   Se usa calloc para que quede inicializado en ceros de una vez. */
void documentoInicializar(DocumentoVentana *documento)
{
    int i;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        documento->vectores[i] = calloc((size_t)diccionarios[i].cantidadPalabras, sizeof(int));

        if (!documento->vectores[i])
        {
            fprintf(stderr, "Sin memoria para el vector de frecuencias.\n");
        }
    }
}

void documentoLiberar(DocumentoVentana *documento)
{
    int i;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        free(documento->vectores[i]);
        documento->vectores[i] = NULL;
    }
}

void documentoAgregarLinea(DocumentoVentana *documento, char linea[])
{
    int i;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        if (documento->vectores[i] != NULL)
        {
            acumularVector(linea, &diccionarios[i], documento->vectores[i]);
        }
    }
}

static int sumaVector(const int vector[], int cantidad)
{
    int suma = 0;
    int i;

    if (!vector)
    {
        return 0;
    }

    for (i = 0; i < cantidad; i++)
    {
        suma += vector[i];
    }

    return suma;
}

/* Aplica, sobre el documento COMPLETO de la ventana (con todas sus
   lineas ya acumuladas), las reglas del enunciado:
   - Si al menos 3 palabras de un diccionario aparecen en el
     documento, el documento pertenece a esa clase.
   - Si mas de un diccionario cumple esa condicion, gana el que tenga
     la mayor suma de frecuencias. */
int documentoClasificar(const DocumentoVentana *documento)
{
    int mejorTipo = DESCONOCIDO;
    int mayorFrecuencia = 0;
    int i;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        int frecuencia = sumaVector(documento->vectores[i], diccionarios[i].cantidadPalabras);

        if (frecuencia >= 3 && frecuencia > mayorFrecuencia)
        {
            mayorFrecuencia = frecuencia;
            mejorTipo = i;
        }
    }

    return mejorTipo;
}

void documentoImprimirResumen(const DocumentoVentana *documento)
{
    int i, suma;

    printf("  Frecuencias acumuladas por diccionario:\n");

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        suma = sumaVector(documento->vectores[i], diccionarios[i].cantidadPalabras);
        printf("    %-20s total=%d\n", NOMBRES_CATEGORIAS[i], suma);
    }
}

const char *nombreClase(int tipo)
{
    if (tipo == DESCONOCIDO)
    {
        return "Documento desconocido";
    }

    return NOMBRES_CATEGORIAS[tipo];
}
