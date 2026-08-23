#ifndef IALEARNER_H
#define IALEARNER_H

/* Nada de este archivo lo consume otro .c del proyecto (solo lo
   incluye ialearner.c a si mismo) -- por eso el header se mantiene
   minimo: casi todo el estado (contadores, matrices, registros por
   launcher) es privado (static) dentro de ialearner.c, protegido con
   sus propios mutex internos. */

void mostrarResumenFinal(void);
void *atenderCliente(void *arg);

#endif
