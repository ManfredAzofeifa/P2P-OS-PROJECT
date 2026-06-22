/*
 * hash.c - Funcion de hash propia basada en el contenido del archivo.
 */

#include "hash.h"

#include <errno.h>
#include <stdio.h>

#define P2P_HASH_OFFSET_A 1469598103934665603ULL
#define P2P_HASH_OFFSET_B 1099511628211ULL
#define P2P_HASH_PRIME_A 1099511628211ULL
#define P2P_HASH_PRIME_B 14029467366897019727ULL

// Rota los bits de un valor de 64 bits hacia la izquierda una cantidad dada de posiciones.
// Recibe: el valor sobre el que operar, y cuantas posiciones rotar.
// Devuelve: el valor con sus bits rotados, usado internamente para mezclar el estado del hash.
static uint64_t rotar_izquierda64(uint64_t value, unsigned int bits) {
    return (value << bits) | (value >> (64U - bits));
}

// Inicializa el estado del hash con los valores de offset predefinidos, listos para recibir datos.
// Recibe: el estado del hash a inicializar.
// Devuelve: nada; modifica el estado en el lugar.
void iniciar_hash_p2p(hash_p2p_t *hash) {
    if (hash == NULL) {
        return;
    }

    hash->high = P2P_HASH_OFFSET_A;
    hash->low = P2P_HASH_OFFSET_B;
}

// Incorpora un bloque de bytes al estado del hash, mezclando cada byte con rotaciones y multiplicaciones.
// Recibe: el estado del hash acumulado hasta ahora, los bytes a incorporar, y la cantidad de bytes a procesar.
// Devuelve: nada; actualiza el estado del hash en el lugar.
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

// Convierte el estado final del hash en una cadena hexadecimal de 32 caracteres.
// Recibe: el estado del hash ya finalizado, y el buffer donde escribir la cadena hexadecimal.
// Devuelve: nada; escribe el identificador del archivo en el buffer de salida.
void cerrar_hash_hex_p2p(const hash_p2p_t *hash, char out[P2P_HASH_STR_LEN]) {
    if (hash == NULL || out == NULL) {
        return;
    }

    snprintf(out, P2P_HASH_STR_LEN, "%016llx%016llx",
             (unsigned long long)hash->high,
             (unsigned long long)hash->low);
}

// Calcula el hash de un archivo leyendolo completo en bloques y produce una cadena hexadecimal identificadora.
// Recibe: la ruta del archivo a hashear, y el buffer donde guardar el hash resultante.
// Devuelve: 0 si el hash se calculo correctamente, -1 si el archivo no se pudo abrir, leer, o cerrar.
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