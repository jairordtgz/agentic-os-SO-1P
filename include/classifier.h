#ifndef CLASSIFIER_H
#define CLASSIFIER_H

#define EMAIL 0
#define ARTICULO 1
#define REPORTE 2
#define DESCONOCIDO -1

#define NUM_DICCIONARIOS 3

/* TDA que representa UN documento (una ventana completa).
   Guarda la suma de frecuencias de palabras de cada diccionario,
   acumulada a lo largo de TODAS las lineas que el usuario escribio
   en esa ventana.
*/

typedef struct
{
    int *vectores[NUM_DICCIONARIOS];
} DocumentoVentana;

/* Carga los 3 diccionarios desde archivos de texto dentro de
  la carpeta diccionarios (correo.txt, articulo.txt, reporte.txt)

*/
int cargarDiccionarios(const char *carpeta);

/* Libera la memoria reservada por la tabla hash de cada diccionario */
void liberarDiccionarios(void);

/* documentoInicializar reserva memoria (calloc) para el vector de
   cada diccionario, del tamaño exacto que se cargo en cargarDiccionarios.
   documentoLiberar la libera  */

void documentoInicializar(DocumentoVentana *documento);
void documentoLiberar(DocumentoVentana *documento);

void documentoAgregarLinea(DocumentoVentana *documento, char linea[]);
int documentoClasificar(const DocumentoVentana *documento);
void documentoImprimirResumen(const DocumentoVentana *documento);

const char *nombreClase(int tipo);

#endif
