# Simulador P2P — IC-6600 Principios de Sistemas Operativos

Proyecto 2 · I Semestre 2026 · Instituto Tecnológico de Costa Rica

## Descripción

Simulación de un sistema P2P que se comunica a través de sockets para localizar y transferir archivos en red. Incluye búsqueda centralizada (vía servidor) y búsqueda distribuida (vía vecinos con TTL).

## Integrantes

| Persona | Módulo responsable |
|---|---|
| Persona 1 | Protocolo, hash, servidor P2P |
| Persona 2 | Cliente, transferencia, armado de archivos |
| Persona 3 | Búsqueda distribuida, vecinos, control de propagación |

## Estructura del proyecto

```
p2p-simulator/
├── server/        # Servidor P2P y función de hash
├── client/        # Cliente P2P, consola y transferencia
├── distributed/   # Búsqueda distribuida y vecinos
├── protocol/      # Protocolo de comunicación compartido
├── docs/          # Documentación en LaTeX
└── tests/         # Scripts de prueba
```

## Compilar

```bash
make        # compila servidor y cliente
make clean  # elimina binarios
```

## Uso

```bash
# Levantar servidor
./bin/server <puerto>

# Levantar cliente
./bin/client <ip_servidor> <puerto_servidor> <puerto_propio> <carpeta_compartida>

# Comandos del cliente
find -s <nombre>   # búsqueda centralizada
find -d <nombre>   # búsqueda distribuida
find <nombre>      # intenta centralizada, cae a distribuida si el servidor no responde
request <S> <H>    # solicitar archivo por tamaño y hash
```

## Cronograma

| Semana | Fechas | Hito |
|---|---|---|
| S1–S2 | Sep 22 – Oct 5 | Protocolo definido, hash implementado, cliente base |
| S3–S4 | Oct 6 – Oct 19 | Servidor completo, transferencia por segmentos |
| S4–S6 | Oct 13 – Nov 2 | Búsqueda distribuida, vecinos, TTL |
| S5–S7 | Oct 20 – Nov 9 | Integración, pruebas en red real (LAIIMI) |
| S6–S8 | Oct 27 – Nov 16 | Documentación LaTeX, correcciones finales |
| **S8** | **Nov 19** | **Entrega final** |

## Entrega

- Formato: PDF de la documentación + código al correo `edramirez@itcr.ac.cr`
- Asunto: `IC-6600 Proyecto 2 [Nombres de los integrantes]`
- El programa debe correr en Linux estándar con clientes en la misma red.
