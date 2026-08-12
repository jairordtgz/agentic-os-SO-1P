#ifndef CLASSIFIER_H
#define CLASSIFIER_H

#define EMAIL 0
#define ARTICULO 1
#define REPORTE 2
#define DESCONOCIDO -1

#define NUM_DICCIONARIOS 3

/* Tope MAXIMO de palabras por diccionario (tamaño del arreglo). La
   cantidad REAL de palabras se lee de cada archivo .txt en tiempo de
   ejecucion (ver cargarDiccionarios) y puede ser menor que este tope.
   Aumentar este numero no requiere cambiar la logica, solo agranda
   el espacio disponible por si se agregan muchas palabras nuevas. */
#define MAX_PALABRAS 100

/* TDA que representa UN documento (= una ventana completa).
   Guarda la suma de frecuencias de palabras de cada diccionario,
   acumulada a lo largo de TODAS las lineas que el usuario escribio
   en esa ventana. Esto es la "matriz de vectores de frecuencia de
   las lineas de texto" del enunciado, ya reducida a la suma por
   columna de cada diccionario. */
typedef struct
{
    int vectores[NUM_DICCIONARIOS][MAX_PALABRAS];
} DocumentoVentana;

/* Carga los 3 diccionarios desde archivos de texto dentro de
   "carpeta" (correo.txt, articulo.txt, reporte.txt). Construye,
   ademas, una tabla hash por diccionario para que buscar si una
   palabra pertenece a el sea O(1) en promedio, en vez de recorrer
   la lista completa de palabras con un for.

   Se debe llamar UNA sola vez, al iniciar el programa, ANTES de
   aceptar cualquier conexion -- los hilos solo LEEN esta estructura
   despues de cargada, nunca la modifican, asi que no hace falta
   protegerla con mutex una vez que arranco el servidor.

   Devuelve 0 si tuvo exito, -1 si algun archivo no se pudo leer. */
int cargarDiccionarios(const char *carpeta);

/* Libera la memoria reservada por la tabla hash de cada diccionario. */
void liberarDiccionarios(void);

void documentoInicializar(DocumentoVentana *documento);
void documentoAgregarLinea(DocumentoVentana *documento, char linea[]);
int documentoClasificar(const DocumentoVentana *documento);
void documentoImprimirResumen(const DocumentoVentana *documento);

const char *nombreClase(int tipo);

#endif
