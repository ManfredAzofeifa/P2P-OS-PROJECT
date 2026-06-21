#ifndef TRANSFER_H
#define TRANSFER_H

#include "client.h"

int iniciar_servidor_transferencia(const contexto_cliente_p2p_t *contexto);
int descargar_de_par(const contexto_cliente_p2p_t *contexto,
                                const punto_red_p2p_t *par,
                                uint64_t tamano,
                                const char *hash,
                                char *ruta_guardada,
                                size_t tamano_ruta_guardada,
                                int *ya_existe);
int descargar_de_pares(const contexto_cliente_p2p_t *contexto,
                                 const punto_red_p2p_t *pares,
                                 size_t cantidad_pares,
                                 uint64_t tamano,
                                 const char *hash,
                                 char *ruta_guardada,
                                 size_t tamano_ruta_guardada,
                                 int *ya_existe,
                                 int *segmentado);

#endif
