#ifndef TRANSFER_H
#define TRANSFER_H

#include "client.h"

/* Abre el puerto P2P del contexto y deja un hilo atendiendo GET/DSEARCH/DRESULT. */
int iniciar_servidor_transferencia(const contexto_cliente_p2p_t *contexto);

/* Descarga y verifica un archivo completo desde un par; informa ruta y existencia previa. */
int descargar_de_par(const contexto_cliente_p2p_t *contexto,
                                const punto_red_p2p_t *par,
                                uint64_t tamano,
                                const char *hash,
                                char *ruta_guardada,
                                size_t tamano_ruta_guardada,
                                int *ya_existe);

/* Descarga y verifica desde uno o varios pares, con rangos y recuperacion ante fallos. */
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
