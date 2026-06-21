/*
 * hash.c - Funcion de hash propia basada en el contenido del archivo.
 *
 * Documentacion de funciones:
 * rotar_izquierda64: hace una rotacion de bits; recibe un valor y bits; ayuda al hash propio.
 * iniciar_hash_p2p: inicia el estado del hash; recibe el estado; ayuda a identificar archivos por contenido.
 * actualizar_hash_p2p: mezcla bytes al hash; recibe estado, datos y largo; calcula el hash sin bibliotecas.
 * cerrar_hash_hex_p2p: pasa el hash a hexadecimal; recibe estado y salida; produce el identificador del archivo.
 * calcular_hash_archivo: calcula el hash de un archivo; recibe ruta y salida; permite reconocer archivos aunque cambien de nombre.
 */

#include "hash.h"

#include <errno.h>
#include <stdio.h>

#define P2P_HASH_OFFSET_A 1469598103934665603ULL
#define P2P_HASH_OFFSET_B 1099511628211ULL
#define P2P_HASH_PRIME_A 1099511628211ULL
#define P2P_HASH_PRIME_B 14029467366897019727ULL

static uint64_t rotar_izquierda64(uint64_t value, unsigned int bits) {
    return (value << bits) | (value >> (64U - bits));
}

void iniciar_hash_p2p(hash_p2p_t *hash) {
    if (hash == NULL) {
        return;
    }

    hash->high = P2P_HASH_OFFSET_A;
    hash->low = P2P_HASH_OFFSET_B;
}

void actualizar_hash_p2p(hash_p2p_t *hash, const unsigned char *datos, size_t len) {
    size_t i;

    if (hash == NULL || datos == NULL) {
        return;
    }

    for (i = 0; i < len; i++) {
        unsigned char byte = datos[i];

        hash->high ^= (uint64_t)byte;
        hash->high *= P2P_HASH_PRIME_A;
        hash->high = rotar_izquierda64(hash->high, 13);

        hash->low += (uint64_t)byte + (hash->high & 0xffU);
        hash->low ^= rotar_izquierda64(hash->low, 17);
        hash->low *= P2P_HASH_PRIME_B;
    }
}

void cerrar_hash_hex_p2p(const hash_p2p_t *hash, char out[P2P_HASH_STR_LEN]) {
    if (hash == NULL || out == NULL) {
        return;
    }

    snprintf(out, P2P_HASH_STR_LEN, "%016llx%016llx",
             (unsigned long long)hash->high,
             (unsigned long long)hash->low);
}

int calcular_hash_archivo(const char *ruta, char out[P2P_HASH_STR_LEN]) {
    FILE *archivo;
    hash_p2p_t hash;
    unsigned char buffer[8192];
    size_t nread;

    if (ruta == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    archivo = fopen(ruta, "rb");
    if (archivo == NULL) {
        return -1;
    }

    iniciar_hash_p2p(&hash);
    while ((nread = fread(buffer, 1, sizeof(buffer), archivo)) > 0) {
        actualizar_hash_p2p(&hash, buffer, nread);
    }

    if (ferror(archivo)) {
        fclose(archivo);
        return -1;
    }

    if (fclose(archivo) != 0) {
        return -1;
    }

    cerrar_hash_hex_p2p(&hash, out);
    return 0;
}
