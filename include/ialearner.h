#ifndef IALEARNER_H
#define IALEARNER_H
#include <pthread.h>

extern int documentosCorreo;
extern int documentosArticulo;
extern int documentosReporte;
extern int clientesConectados;

extern pthread_mutex_t mutex;

void actualizarResumen(int tipo);
void mostrarResumenFinal(void);

void *atenderCliente(void *arg);

#endif
