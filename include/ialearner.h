#ifndef IALEARNER_H
#define IALEARNER_H

#include <pthread.h>
#include "classifier.h"

extern int documentosCorreo;
extern int documentosArticulo;
extern int documentosReporte;
extern int clientesConectados;

/* Matriz de frecuencias GLOBAL: a diferencia del "documento" que
   cada hilo arma para SU ventana (que es local y no necesita mutex),
   esta si es una sola copia compartida por TODOS los hilos, que se
   va incrementando en tiempo real conforme cada hilo encuentra
   palabras en cualquier ventana. Por eso toda escritura sobre esta
   variable debe hacerse con el mutex tomado. */
extern DocumentoVentana matrizGlobal;

extern pthread_mutex_t mutex;

void actualizarResumen(int tipo);
void mostrarResumenFinal(void);

void *atenderCliente(void *arg);

#endif
