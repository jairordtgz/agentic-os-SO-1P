#ifndef CLASSIFIER_H
#define CLASSIFIER_H

#define EMAIL 0
#define ARTICULO 1
#define REPORTE 2
#define DESCONOCIDO -1

#define NUM_DICCIONARIOS 3

/* TDA que representa UN documento (= una ventana completa).
   Guarda la suma de frecuencias de palabras de cada diccionario,
   acumulada a lo largo de TODAS las lineas que el usuario escribio
   en esa ventana. Esto es la "matriz de vectores de frecuencia de
   las lineas de texto" del enunciado, ya reducida a la suma por
   columna de cada diccionario.

   Cada fila (vectores[i]) se reserva dinamicamente con exactamente
   tantas posiciones como palabras tenga el diccionario i -- no hay
   ningun tope maximo de palabras por diccionario: un archivo .txt
   puede tener 10 palabras o 10000, el vector se ajusta solo. */
typedef struct
{
    int *vectores[NUM_DICCIONARIOS];
} DocumentoVentana;

/* Carga los 3 diccionarios desde archivos de texto dentro de
   "carpeta" (correo.txt, articulo.txt, reporte.txt). Cada archivo
   tiene UNA palabra por linea, sin ningun encabezado. Construye,
   ademas, una tabla hash por diccionario para que buscar si una
   palabra pertenece a el sea O(1) en promedio, sin importar cuantas
   palabras tenga.

   Se debe llamar UNA sola vez, al iniciar el programa, ANTES de
   aceptar cualquier conexion -- los hilos solo LEEN esta estructura
   despues de cargada, nunca la modifican, asi que no hace falta
   protegerla con mutex una vez que arranco el servidor.

   Devuelve 0 si tuvo exito, -1 si algun archivo no se pudo leer. */
int cargarDiccionarios(const char *carpeta);

/* Libera la memoria reservada por la tabla hash de cada diccionario
   (se llama una vez, al cerrar el programa). */
void liberarDiccionarios(void);

/* documentoInicializar reserva memoria (calloc) para el vector de
   cada diccionario, del tamaño exacto que se cargo en cargarDiccionarios.
   documentoLiberar la libera -- se debe llamar por cada DocumentoVentana
   que ya no se vaya a usar (por ejemplo, al cerrar una ventana), para
   no dejar fugas de memoria por cada conexion atendida. */
void documentoInicializar(DocumentoVentana *documento);
void documentoLiberar(DocumentoVentana *documento);

void documentoAgregarLinea(DocumentoVentana *documento, char linea[]);
int documentoClasificar(const DocumentoVentana *documento);
void documentoImprimirResumen(const DocumentoVentana *documento);

const char *nombreClase(int tipo);

#endif
