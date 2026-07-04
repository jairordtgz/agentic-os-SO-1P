#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "classifier.h"

static Diccionario diccionarios[NUM_DICCIONARIOS] =
{
    {
        "Correo electronico",
        {
            "thank", "please", "regards", "meeting", "attached",
            "information", "update", "schedule", "team", "project"
        }
    },
    {
        "Articulo cientifico",
        {
            "data", "analysis", "results", "method", "study",
            "model", "research", "system", "significant", "effect"
        }
    },
    {
        "Reporte",
        {
            "system", "data", "network", "security", "application",
            "server", "user", "performance", "service", "infrastructure"
        }
    }
};

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

static int buscarPalabra(const Diccionario *dic, const char palabra[])
{
    int i;

    for (i = 0; i < PALABRAS; i++)
    {
        if (strcmp(dic->palabras[i], palabra) == 0)
        {
            return i;
        }
    }

    return -1;
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

    /* strncpy + terminador manual: si el texto es mas largo que el
       buffer local, se trunca en vez de desbordar memoria. */
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

void documentoInicializar(DocumentoVentana *documento)
{
    int i, j;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        for (j = 0; j < PALABRAS; j++)
        {
            documento->vectores[i][j] = 0;
        }
    }
}

void documentoAgregarLinea(DocumentoVentana *documento, char linea[])
{
    int i;

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        acumularVector(linea, &diccionarios[i], documento->vectores[i]);
    }
}

static int sumaVector(const int vector[])
{
    int suma = 0;
    int i;

    for (i = 0; i < PALABRAS; i++)
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
        int frecuencia = sumaVector(documento->vectores[i]);

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
    int i, j, suma;

    printf("  Frecuencias acumuladas por diccionario:\n");

    for (i = 0; i < NUM_DICCIONARIOS; i++)
    {
        suma = 0;

        for (j = 0; j < PALABRAS; j++)
        {
            suma += documento->vectores[i][j];
        }

        printf("    %-20s total=%d\n", diccionarios[i].nombre, suma);
    }
}

const char *nombreClase(int tipo)
{
    if (tipo == DESCONOCIDO)
    {
        return "Documento desconocido";
    }

    return diccionarios[tipo].nombre;
}
