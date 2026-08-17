#ifndef IALEARNER_H
#define IALEARNER_H

#include <pthread.h>
#include "classifier.h"

extern int documentosCorreo;
extern int documentosArticulo;
extern int documentosReporte;
extern int clientesConectados;

/* Matriz de frecuencias GLOBAL: todos los hilos de deteccion escriben
   aqui conforme procesan oraciones (de cualquier ventana). Es la
   unica pieza de este tipo que de verdad comparten -- por eso, y
   solo por eso, se protege con mutex. */
extern DocumentoVentana matrizGlobal;

extern pthread_mutex_t mutex;

void mostrarResumenFinal(void);

void *atenderCliente(void *arg);

#endif
