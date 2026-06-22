# Simulador P2P de archivos

Proyecto de Sistemas Operativos hecho en C para simular busqueda y descarga de archivos entre clientes P2P usando sockets y `pthread` en Linux.

## Objetivo

El sistema permite que varios clientes compartan una carpeta. Al iniciar, cada cliente registra en un servidor central los archivos que tiene, junto con:

- hash propio del contenido
- tamano
- IP del cliente
- puerto de transferencia

Luego un usuario puede buscar archivos por nombre, buscarlos de forma distribuida por vecinos, y pedir una descarga por tamano y hash.

## Requisitos cubiertos

- Funcion hash propia, sin bibliotecas externas.
- Servidor P2P con metadatos de archivos.
- Cliente P2P con argumentos por consola.
- Registro automatico de archivos de la carpeta compartida.
- `find -s <nombre>` para busqueda centralizada.
- `find -d <nombre>` para busqueda distribuida por vecinos.
- `find <nombre>` para intentar servidor y luego distribuida.
- `request <S> <H>` para descargar por tamano y hash.
- Transferencia por rangos de bytes.
- Descarga segmentada usando varios pares cuando hay mas de un candidato.
- Reintento de rangos si un par falla.
- Reensamblado del archivo descargado.
- Busqueda distribuida con TTL, ID de consulta, cache de vistos y expiracion.
- Manejo seguro cuando la carpeta compartida no existe o se desconecta.

## Compilar

```bash
make
```

Esto genera:

```text
bin/server
bin/client
```

Limpiar binarios:

```bash
make clean
```

## Ejecutar

Servidor:

```bash
./bin/server <puerto>
```

Cliente:

```bash
./bin/client <ip_servidor> <puerto_servidor> <puerto_transferencia> <carpeta_compartida> [ip_anunciada]
```

`ip_anunciada` es opcional. Se usa cuando el cliente esta detras de NAT y los
otros pares deben conectarse a una IPv4 distinta de la que observa el servidor.

Ejemplo:

```bash
./bin/server 39090
./bin/client 127.0.0.1 39090 41001 /tmp/p2p-a
./bin/client 127.0.0.1 39090 41002 /tmp/p2p-b
```

### Computadora y VM con adaptador puente (recomendado)

`127.0.0.1` solo sirve cuando servidor y cliente corren en la misma maquina. Para
dos maquinas, ambos clientes deben usar la IP LAN de la computadora que ejecuta
el servidor, por ejemplo `192.168.1.20`:

```bash
# Computadora normal (IP 192.168.1.20)
./bin/server 39090
./bin/client 192.168.1.20 39090 41001 Carpetas/Carpeta1

# VM en otra terminal (por ejemplo, IP 192.168.1.30)
./bin/client 192.168.1.20 39090 41002 Carpetas/Carpeta2
```

La maquina virtual debe usar red **puente (bridged)** para obtener una IP
alcanzable desde la computadora. En este modo no hace falta `ip_anunciada`: el
servidor observa y publica `192.168.1.30` automaticamente.

El firewall de la computadora debe permitir TCP `39090` y `41001`; el de la VM
debe permitir TCP `41002`. El servidor y los clientes escuchan en todas las
interfaces IPv4 (`0.0.0.0`).

Comprobacion minima desde la otra maquina:

```bash
nc -vz 192.168.1.20 39090   # puerto central
nc -vz <ip-del-cliente> 41001  # puerto P2P del cliente
```

### VM con NAT y reenvio de puerto

Si no se puede usar modo puente, hay que reenviar el puerto P2P del host hacia
la VM y usar el nuevo argumento `ip_anunciada`. Para VirtualBox, con la VM
apagada y reemplazando `NombreVM`:

```bash
VBoxManage modifyvm "NombreVM" --natpf1 "p2p-vm,tcp,,41002,,41002"
```

Suponiendo que el servidor corre en el host, que la IP LAN del host es
`192.168.1.20` y que VirtualBox lo expone dentro de la VM como `10.0.2.2`:

```bash
# Dentro de la VM: el ultimo argumento es la IP que se publicara a los pares
./bin/client 10.0.2.2 39090 41002 Carpetas/Carpeta2 192.168.1.20
```

El puerto externo y el puerto P2P deben coincidir (`41002` en el ejemplo). Si se
usa VMware, QEMU u otro hipervisor, se configura el mismo mapeo TCP en su interfaz
y se pasa como `ip_anunciada` la IPv4 del host que recibe ese puerto. Cada cliente
detras del mismo NAT necesita un puerto externo diferente.

Al arrancar, comprobar que el cliente imprime el endpoint esperado:

```text
endpoint P2P anunciado: 192.168.1.20:41002
```

### Replicar una descarga de Carpeta1 a Carpeta2

Desde la raiz del proyecto, preparar las carpetas y un archivo fuente:

```bash
mkdir -p Carpetas/Carpeta1 Carpetas/Carpeta2
printf 'archivo para probar P2P\n' > Carpetas/Carpeta1/prueba.txt
make
```

Abrir tres terminales. En la primera:

```bash
./bin/server 39090
```

En la segunda, iniciar el cliente que comparte `Carpeta1`:

```bash
./bin/client 127.0.0.1 39090 41001 Carpetas/Carpeta1
```

En su consola ejecutar `archivos` y copiar el tamano y hash de `prueba.txt`:

```text
p2p> archivos
24 <HASH_MOSTRADO> prueba.txt
```

En la tercera terminal, iniciar el cliente de destino:

```bash
./bin/client 127.0.0.1 39090 41002 Carpetas/Carpeta2
```

Buscar y descargar usando el tamano y hash exactos mostrados por `archivos`:

```text
p2p> find -s prueba.txt
p2p> request 24 <HASH_MOSTRADO>
```

El destino usa el hash como nombre. Verificar el contenido reemplazando el hash:

```bash
cmp Carpetas/Carpeta1/prueba.txt Carpetas/Carpeta2/<HASH_MOSTRADO>
```

Para repetirlo entre computadora y VM, se usan los mismos comandos pero con las
IPs y puertos de las secciones anteriores; `Carpeta1` puede estar en el host y
`Carpeta2` dentro de la VM.

## Comandos del cliente

### `archivos`

Muestra los archivos locales registrados por el cliente:

```text
<tamano> <hash> <nombre>
```

Sirve para copiar el tamano y hash que luego se usan con `request`.

### `vecinos`

Muestra los vecinos que el servidor devolvio al momento del registro.

Antes de mostrarlos, el cliente renueva su registro para incluir clientes que
hayan arrancado despues. Estos vecinos recientes se usan para `find -d`, que
tambien hace esa actualizacion automaticamente.

### `find -s <nombre>`

Hace busqueda centralizada.

Como se resuelve:

1. El cliente abre una conexion TCP al servidor.
2. Envia:
   ```text
   FIND <nombre>
   ```
3. El servidor revisa sus metadatos registrados.
4. Si encuentra archivos con ese nombre, responde:
   ```text
   PEERS <cantidad>
   PEER <ip> <puerto>
   END
   ```
5. El cliente imprime los pares encontrados.

Este comando busca por nombre, no por hash.

### `find -d <nombre>`

Hace busqueda distribuida.

Como se resuelve:

1. El cliente genera un ID de consulta.
2. Envia `DSEARCH` a sus vecinos.
3. Cada vecino revisa si ya vio ese ID.
4. Si ya lo vio, lo descarta.
5. Si tiene un archivo cuyo nombre calza, responde directo al originador con `DRESULT`.
6. Si no se agoto el TTL, reenvia la busqueda a sus vecinos.
7. El originador junta los resultados recibidos por un tiempo corto.

Esto demuestra inundacion controlada por TTL y evita ciclos con el cache de consultas vistas.

### `find <nombre>`

Intenta primero `find -s <nombre>`.

Si no logra resolverlo por servidor, usa busqueda distribuida como respaldo.

### `request <S> <H>`

Descarga un archivo por tamano y hash.

Como se resuelve:

1. El cliente pregunta al servidor:
   ```text
   LOOKUP <S> <H>
   ```
2. El servidor responde los pares que tienen ese tamano y hash.
3. Si hay un solo par, el cliente descarga desde ese par.
4. Si hay varios pares, divide el archivo en rangos.
5. Cada rango se pide con:
   ```text
   GET <S> <H> <inicio> <largo>
   ```
6. El par responde:
   ```text
   DATA <largo>
   <bytes>
   ```
7. El cliente reensambla el archivo.
8. El archivo final se guarda como:
   ```text
   <carpeta_compartida>/<hash>
   ```

Se guarda con el hash como nombre porque `request <S> <H>` no incluye nombre de archivo.

## Protocolo resumido

Cliente a servidor:

```text
REGISTER <puerto_transferencia> <cantidad_archivos> [ip_anunciada]
FILE <tamano> <hash> <nombre>
END
FIND <nombre>
LOOKUP <tamano> <hash>
```

Servidor a cliente:

```text
OK <texto>
ERROR <texto>
PEERS <cantidad>
PEER <ip> <puerto>
NEIGHBORS <cantidad>
NEIGHBOR <ip> <puerto>
END
```

Cliente a cliente:

```text
GET <tamano> <hash> <inicio> <largo>
DATA <largo>
<bytes>
ERROR <texto>
DSEARCH <id_consulta> <ip_origen> <puerto_origen> <ttl> <termino>
DRESULT <id_consulta> <tamano> <hash> <nombre> <ip_dueno> <puerto_dueno>
```

## Pruebas automaticas

Ejecutar todo:

```bash
make test
```

Incluye:

- prueba de hash propio
- registro y busqueda en servidor
- registro de cliente real
- `find -s`
- `find -d`
- TTL y descarte de duplicados
- descarga por `request`
- descarga segmentada
- reintento cuando un par falla
- manejo de pares caidos
- verificacion de rangos `GET`
- prueba con seis clientes reales en una topologia con ciclos

Tambien se pueden correr por separado:

```bash
make test-hash
make test-server
make test-client
```

Las pruebas abren puertos locales. Si el ambiente bloquea sockets locales, correrlas en una terminal normal de Linux.

## Guia manual para demostrar que funciona

### 1. Preparar carpetas

```bash
mkdir -p /tmp/p2p-a /tmp/p2p-b /tmp/p2p-c
printf 'hola desde a\n' > /tmp/p2p-a/hola.txt
printf 'otro archivo\n' > /tmp/p2p-b/otro.txt
```

### 2. Compilar

```bash
make
```

### 3. Levantar servidor

Terminal 1:

```bash
./bin/server 39090
```

### 4. Levantar clientes

Terminal 2:

```bash
./bin/client 127.0.0.1 39090 41001 /tmp/p2p-a
```

Terminal 3:

```bash
./bin/client 127.0.0.1 39090 41002 /tmp/p2p-b
```

Terminal 4:

```bash
./bin/client 127.0.0.1 39090 41003 /tmp/p2p-c
```

### 5. Ver archivos registrados

En el cliente de `/tmp/p2p-a`:

```text
archivos
```

Copiar el tamano y hash de `hola.txt`.

### 6. Probar busqueda centralizada

En cualquier cliente:

```text
find -s hola.txt
```

Debe mostrar el IP y puerto del cliente que tiene `hola.txt`.

### 7. Probar busqueda distribuida

En un cliente que tenga vecinos:

```text
vecinos
find -d hola.txt
```

Debe mostrar resultados si el archivo esta en algun vecino alcanzable por TTL.

### 8. Probar descarga

Usar el tamano y hash que salieron con `archivos`:

```text
request <tamano> <hash>
```

El archivo descargado queda en la carpeta compartida del cliente que pidio la descarga, con el hash como nombre.

Ejemplo para revisar:

```bash
ls -l /tmp/p2p-c
cat /tmp/p2p-c/<hash>
```

### 9. Probar resistencia con carpeta faltante

```bash
./bin/client 127.0.0.1 39090 41004 /tmp/no-existe
```

El cliente debe avisar que no puede abrir la carpeta, registrar cero archivos y no caerse.

## Estructura

```text
client/        cliente, consola y transferencia
distributed/   busqueda distribuida, TTL y cache de consultas
protocol/      constantes y mensajes compartidos
server/        servidor, metadatos y hash propio
tests/         pruebas automaticas
docs/          documentacion LaTeX del curso
```
