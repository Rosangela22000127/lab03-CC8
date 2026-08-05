# Concepto: qué hace `lab_3.c`

Explicación conceptual del servidor, sin entrar al detalle línea por línea (ese mapa
está en [`guia.md`](guia.md), sección *"Dónde está cada cosa en `lab_3.c`"*).

---

## La idea en una frase

`lab_3.c` es un **resolver DNS recursivo con caché**: recibe preguntas del tipo
*"¿cuál es la IP de `google.com`?"* por UDP, y las contesta desde su propia memoria
si ya las sabe, o le pregunta a un DNS público si no.

No es un proxy que reenvía bytes: **cada respuesta que sale se arma byte por byte
dentro del programa**, a partir de datos guardados en la caché.

---

## Qué es DNS, en corto

Internet funciona con IPs (`142.250.113.100`), pero la gente escribe nombres
(`google.com`). DNS es la traducción entre los dos.

Cuando algo en tu máquina necesita resolver un nombre, manda **un paquete UDP al
puerto 53** del servidor DNS configurado. Ese paquete es binario y tiene una
estructura fija (RFC 1035):

```
+-----------------+  12 bytes: ID, flags (QR/RD/RA/RCODE), y cuántos
|     HEADER      |  registros trae cada una de las 4 secciones
+-----------------+
|    QUESTION     |  qué se pregunta: nombre + tipo (A, MX, ...) + clase
+-----------------+
|     ANSWER      |  la respuesta
+-----------------+
|    AUTHORITY    |  qué servidores mandan sobre esa zona (NS / SOA)
+-----------------+
|   ADDITIONAL    |  datos extra útiles (las IPs de esos NS)
+-----------------+
```

Un nombre no se escribe como texto plano: va como **labels con prefijo de largo** —
`www.github.com` viaja como `3www 6github 3com 0`. Y para ahorrar espacio, un nombre
puede ser un **puntero** a un offset anterior del mismo paquete en vez de repetirse
(compresión). Todo eso se lee y se escribe a mano en este código; no hay librería
que lo haga.

---

## Las cinco piezas del programa

### 1. Traductor del formato binario

Leer un paquete que llega y escribir el que sale. Se hace byte por byte, sin `struct`
con bitfields ni casts de memoria, porque el mismo binario tiene que correr igual en
x86 y en el ARM del BeagleBone.

| Función | Qué hace |
|---|---|
| `read_u16` / `read_u32` / `write_u16` / `write_u32` | Enteros en **network byte order** (big-endian), leídos y escritos a mano. La base de todo lo demás. |
| `parse_header` / `write_header` | Los 12 bytes fijos: ID, flags (QR, RD, RA, RCODE) y los 4 contadores de sección. |
| `read_name` | Lee un nombre siguiendo **punteros de compresión** si los hay. Trae un contador `guard` para no colgarse si un paquete malicioso apunta a sí mismo en círculo. |
| `write_name_labels` | Escribe el nombre completo en formato `3www 6github 3com 0`. |
| `write_name_compressed` | Si el nombre es igual al QNAME, escribe solo un puntero de 2 bytes al offset 12 en vez de repetirlo. |
| `skip_rr` | Avanza sobre un Resource Record sin interpretarlo, para llegar al siguiente. |
| `query_edns_udp_size` | Busca el RR `OPT` en Additional para saber cuántos bytes acepta el cliente; si no hay, devuelve el límite clásico de 512. |

### 2. Caché en memoria

Una lista enlazada global (`g_cache`) protegida por un mutex. Cada entrada es
`(nombre, tipo, clase) → RDATA`, más su **TTL** y la hora en que se guardó.

- Arranca vacía y muere con el proceso (no se persiste a disco).
- Una entrada sirve solo si `edad < TTL`; si no, se descarta.
- Al responder se manda el **TTL restante**, no el original, para que el cliente no
  guarde el dato más tiempo del que realmente le queda.
- El RDATA se guarda **estructurado** según el tipo (`A`/`AAAA` como bytes, `CNAME`/`NS`/`PTR`
  como nombre, `MX` como preferencia + nombre, `SOA` con sus 5 campos). Los tipos que
  no se modelan (`TXT`, `HTTPS`, `SVCB`, ...) se guardan como RDATA crudo, lo cual es
  correcto porque esos tipos no comprimen nombres adentro.

| Función | Qué hace |
|---|---|
| `cache_insert` | Guarda un registro con su TTL y la hora actual. Respeta el tope de 4096 entradas para que la caché no crezca sin límite. |
| `cache_lookup` | Devuelve los registros vigentes que matchean `(nombre, tipo, clase)`, saltándose los expirados. |
| `cache_age` / `cache_expired` | Cuánto lleva guardada una entrada y si ya venció. `cache_age` devuelve 0 si el reloj retrocedió, para que una resta negativa no se convierta en un `uint32` gigante que expire todo. |
| `cache_purge_rrset` | Borra un RRset viejo completo antes de reemplazarlo. |
| `rdata_equal` | Compara el RDATA de dos entradas, para descartar duplicados exactos **sin** borrar hermanos del mismo RRset (los 4 `NS` de un dominio comparten nombre/tipo/clase pero son registros distintos, y los 4 deben sobrevivir). |
| `resolve_chain` | La búsqueda "inteligente": si no encuentra el tipo pedido pero sí un `CNAME`, lo agrega a la respuesta y vuelve a buscar sobre el alias (hasta 8 saltos). Marca `complete` solo si llegó a registros del tipo que se pidió. |
| `lookup_up_the_tree` | Sube por la jerarquía del nombre (`www.github.com` → `github.com` → `com`) buscando el `NS` o el `SOA` de la zona, para la sección Authority. |
| `gather_glue` | Junta las `A`/`AAAA` de los nombres citados en Authority y Answer (destinos de `NS`, exchanges de `MX`) para la sección Additional. |

### 3. Recursividad

Si la caché no alcanza, se le manda la consulta a un DNS público (`8.8.8.8`, y
`1.1.1.1` si el primero no contesta en 2s). De la respuesta se **parsean y cachean
todos** los registros — Answer, Authority y Additional — y recién ahí se vuelve a
armar la respuesta desde la caché. Si ninguno contesta, se responde `SERVFAIL` en
vez de dejar al cliente colgado.

| Función | Qué hace |
|---|---|
| `query_with_edns` | Antes de reenviar, le pega EDNS0 a la consulta anunciando nuestro buffer completo, para que el upstream no trunque RRsets grandes aunque el cliente original no use EDNS. |
| `forward_to_upstream` | Manda la consulta al DNS público con timeout de 2s y cae al de respaldo si no contesta. |
| `parse_rr` | Lee **un** Resource Record de la respuesta upstream y lo cachea según su tipo. Tiene dos modos: `RR_PASS_PURGE` (solo invalida el RRset viejo) y `RR_PASS_INSERT` (parsea el RDATA y lo guarda). |
| `cache_response_pass` | Recorre los RR de las tres secciones aplicando uno de esos dos modos. |
| `cache_response` | Llama a lo anterior **dos veces**: primero purga, después inserta. Hacerlo RR por RR borraría los hermanos del mismo RRset entre sí (al llegar el 2do `NS` se eliminaría el 1ro). |

Del lado de armar el paquete que sale:

| Función | Qué hace |
|---|---|
| `write_rr` | Serializa un registro de la caché a bytes, mandando el **TTL restante** en vez del original. |
| `write_opt_rr` | Escribe nuestro propio RR `OPT` (EDNS0) en la respuesta. |
| `write_section` | Escribe una sección entera (Answer, Authority o Additional) y avisa si ya no cabe. |
| `build_response` | Arma el paquete completo: header, Question y las tres secciones. Si no cabe en lo que el cliente acepta, corta y marca `TC=1` en vez de mandar datos a medias. |

### 4. Thread pool

El hilo principal no procesa nada: solo hace `recvfrom()` y **encola** el paquete.
Ocho hilos trabajadores sacan de esa cola y atienden las consultas en paralelo.

Es lo que evita que una consulta lenta (un miss que espera 2s al upstream) bloquee
a todas las demás.

| Función | Qué hace |
|---|---|
| `queue_push` | El hilo principal mete la tarea (paquete + dirección del cliente) en la cola circular. |
| `queue_pop` | El trabajador saca una tarea; si la cola está vacía se **duerme** en una variable de condición en vez de quemar CPU girando. |
| `worker_thread` | El loop de cada uno de los 8 hilos: `queue_pop` → `handle_query` → repetir. |
| `handle_query` | El flujo completo de una consulta (el diagrama de más abajo). |

### 5. Logs estructurados

Cada request/response deja una línea en stdout y en `dns_server.log`: timestamp,
IP:puerto del cliente, `qname`, `qtype`, `action` y latencia.

`action` es lo importante: `CACHE_HIT`, `CACHE_MISS_FORWARD`, `SERVFAIL` o `MALFORMED`.

| Función | Qué hace |
|---|---|
| `log_line` | Escribe la línea en stdout y en el archivo, sin buffering, para que se vea en vivo durante la demo. |
| `qtype_to_str` | Convierte el número del tipo a su nombre (`1` → `A`, `28` → `AAAA`); los que no conoce salen como `TYPE65`. |

---

## El flujo de una consulta

```
paquete UDP llega
      |
  main(): recvfrom  --queue_push-->  cola  --queue_pop-->  worker_thread (1 de 8)
                                                               |
                                                          handle_query()
                                                               |
                    parse_header + read_name  -->  qname, qtype, qclass
                                                               |
                                       resolve_chain()  --- complete ---> CACHE_HIT
                                                               |
                                                          no completo
                                                               |
                                    query_with_edns + forward_to_upstream
                                                               |
                                     cache_response()  (purga + inserta)
                                                               |
                                          resolve_chain() otra vez
                                                               |
                                                     CACHE_MISS_FORWARD
                                                               |
                                  lookup_up_the_tree()  -->  Authority
                                       gather_glue()    -->  Additional
                                                               |
                                                      build_response()
                                                               |
                                                    sendto + log_line()
```

`main()` solo hace el setup: lee puerto y upstream de `argv`, crea el socket UDP,
hace `bind()` (aquí es donde sale *"Address already in use"* si algo ya ocupa el 53),
levanta los 8 hilos y entra al loop de `recvfrom`. `handle_signal()` atrapa `Ctrl+C`
para cerrar ordenadamente.

---

## Detalles que no son obvios (y por qué están)

**Cadenas de CNAME.** `www.github.com` no tiene IP propia: tiene un alias. Contestar
implica devolver el `CNAME` *y además* resolver la `A` del destino. Eso hace
`resolve_chain()`, saltando de alias en alias (máximo 8, para no colgarse).

**Authority y Additional.** Un resolver de verdad no solo contesta la pregunta:
también dice qué servidores mandan sobre esa zona (`lookup_up_the_tree`, subiendo
`www.github.com` → `github.com` → `com`), y adjunta las IPs de esos servidores
(`gather_glue`) para ahorrarle consultas al cliente. Cuando no hay respuesta
(NXDOMAIN), en Authority va el `SOA` de la zona en vez de los `NS`.

**EDNS0 y truncamiento.** El límite clásico de UDP en DNS son 512 bytes. Con EDNS0 el
cliente puede anunciar que acepta más; `query_edns_udp_size` lo lee. Si la respuesta
no cabe, `build_response` marca el flag `TC=1` — que le dice al cliente "reintenta por
TCP" — en vez de mandar datos cortados a la mitad.

**Nada de esto rompe la consola.** Paquetes basura, nombres imposibles de parsear o
upstream caído terminan en una línea de log (`MALFORMED` / `SERVFAIL`) desde
`handle_query`, nunca en un crash ni en un mensaje de error suelto.

---

## Cómo se demuestra que funciona

La misma consulta, dos veces:

```bash
dig @127.0.0.1 -p 5353 google.com A     # CACHE_MISS_FORWARD, ~35 ms
dig @127.0.0.1 -p 5353 google.com A     # CACHE_HIT,          ~1 ms
```

Esa diferencia es la prueba de que hay caché real y de que el paquete de respuesta
se construye adentro del programa. Si fuera un proxy, ambas tardarían lo mismo.
