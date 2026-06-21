#ifndef DISCOVERY_H
#define DISCOVERY_H

#include <stddef.h>
#include <time.h>
#include "../client/client.h"

int manejar_mensaje_par_distribuido(const contexto_cliente_p2p_t *contexto,
                                    const char *linea);
int manejar_mensaje_par_distribuido_en_momento(const contexto_cliente_p2p_t *contexto,
                                       const char *linea,
                                       time_t ahora);
int buscar_en_vecinos(const contexto_cliente_p2p_t *contexto,
                                 const char *termino, metadato_archivo_p2p_t *resultados,
                                 size_t max_resultados, size_t *cantidad_resultados);

#endif
