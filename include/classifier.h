#ifndef CLASSIFIER_H
#define CLASSIFIER_H

#define EMAIL 0
#define ARTICULO 1
#define REPORTE 2
#define DESCONOCIDO -1

#define NUM_DICCIONARIOS 3
#define PALABRAS 10

typedef struct
{
    char *nombre;
    char *palabras[PALABRAS];
} Diccionario;

/* TDA que representa UN documento (= una ventana completa).
   Guarda la suma de frecuencias de palabras de cada diccionario,
   acumulada a lo largo de TODAS las lineas que el usuario escribio
   en esa ventana. Esto es la "matriz de vectores de frecuencia de
   las lineas de texto" del enunciado, ya reducida a la suma por
   columna de cada diccionario. */
typedef struct
{
    int vectores[NUM_DICCIONARIOS][PALABRAS];
} DocumentoVentana;

void documentoInicializar(DocumentoVentana *documento);
void documentoAgregarLinea(DocumentoVentana *documento, char linea[]);
int documentoClasificar(const DocumentoVentana *documento);
void documentoImprimirResumen(const DocumentoVentana *documento);

const char *nombreClase(int tipo);

#endif
