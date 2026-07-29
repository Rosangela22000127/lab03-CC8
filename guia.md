# Guía rápida: correr y probar el servidor DNS en Windows (WSL + Debian)

Pasos para levantar `dns_server` desde cero en una máquina Windows, sin tener que preguntar cada vez.

---

## 0. Preparar WSL con Debian (una sola vez)

En **PowerShell como Administrador**:

```powershell
wsl --install -d Debian
```

Reinicia si te lo pide. Al abrir la ventana de Debian por primera vez, crea tu usuario y contraseña de Linux (no tienen que coincidir con los de Windows).

Dentro de Debian, instala las herramientas de compilación y pruebas (una sola vez):

```bash
sudo apt update && sudo apt install -y build-essential dnsutils
```

- `build-essential` → da `gcc` y `make`.
- `dnsutils` → da `dig`, para mandar consultas de prueba.

---

## 1. Compilar

Abre la ventana **"Debian"** desde el menú inicio de Windows. Entra a la carpeta del proyecto (ojo con las comillas, por el espacio en "Semestre 8"):

```bash
cd "/mnt/c/Users/rosne/Documents/U/Semestre 8/CC8/Labs/Lab03"
```

Verifica que estás en el lugar correcto (`ls` debe mostrar `lab_3.c`, `Makefile`, etc.) y compila:

```bash
make
```

Genera el binario `dns_server`. Es normal ver 1-2 warnings del compilador (no errores) sin que afecte la ejecución.

---

## 2. Probar en un puerto sin privilegios (5353)

No necesita `sudo`. Útil para iterar rápido.

**Ventana 1** (Debian) — deja el servidor corriendo en primer plano:

```bash
./dns_server 5353
```

**Ventana 2** — abre otra ventana "Debian" (menú inicio) y manda consultas:

```bash
dig @127.0.0.1 -p 5353 www.github.com A     # CNAME + A. En la ventana 1 sale: CACHE_MISS_FORWARD
dig @127.0.0.1 -p 5353 www.github.com A     # repite la misma consulta: CACHE_HIT
dig @127.0.0.1 -p 5353 google.com AAAA
dig @127.0.0.1 -p 5353 google.com MX
dig @127.0.0.1 -p 5353 google.com NS
dig @127.0.0.1 -p 5353 google.com SOA
dig @127.0.0.1 -p 5353 -x 8.8.8.8            # PTR (resolución inversa)
```

En la respuesta de `dig`, revisa que diga `status: NOERROR` y que la `ANSWER SECTION` tenga los datos esperados. En la ventana 1 revisa que cada línea de log tenga `qname`, `qtype` y `action` correctos, y que no haya nada en stderr.

Para parar el servidor: `Ctrl+C` en la ventana 1.

---

## 3. Probar en el puerto real 53

El puerto 53 es privilegiado y necesita `sudo`.

**Ventana 1:**

```bash
sudo ./dns_server
```

(usa el puerto 53 por defecto — no hace falta pasarlo como argumento). Te pide tu contraseña de Linux.

**Ventana 2** (ya no se pasa `-p`, porque 53 es el puerto por defecto de `dig`):

```bash
dig @127.0.0.1 google.com A
dig @127.0.0.1 google.com AAAA
```

Si repites la misma consulta una segunda vez deberías ver `action=CACHE_HIT` en el log y un TTL menor que la primera vez (el tiempo que ya pasó desde que se cacheó).

### Si te da "Address already in use" al hacer bind en el puerto 53

Es un choque común con el **DNS tunneling** que trae WSL2 por defecto (un proxy interno que ocupa el puerto 53 dentro de la misma VM Linux). Se revisa así:

```bash
sudo ss -ulpn | grep ':53'
```

Si aparece un listener en una IP tipo `10.255.255.x:53` sin tu proceso, es el de WSL. Para desactivarlo (una sola vez, desde **PowerShell** en Windows, no en Debian):

1. Crea o edita el archivo `%UserProfile%\.wslconfig` (o sea `C:\Users\<tu_usuario>\.wslconfig`) con:
   ```ini
   [wsl2]
   dnsTunneling=false
   ```
2. Cierra todo WSL para que tome el cambio (esto también cierra tus ventanas de Debian abiertas):
   ```powershell
   wsl --shutdown
   ```
3. Vuelve a abrir la ventana "Debian" y repite el paso 3 de esta guía.

---

## 4. (Opcional) Usar tu servidor como DNS primario real de Windows

Esto es lo que pide literalmente el enunciado del lab ("configura tu equipo para que use 127.0.0.1 como DNS primario"). No es necesario para validar que el servidor funciona (el paso 3 ya lo prueba), pero es una demo más vistosa.

1. Deja `sudo ./dns_server` corriendo en la ventana de Debian (WSL2 reenvía automáticamente el puerto de `127.0.0.1` hacia Windows).
2. En Windows: **Panel de Control → Redes e Internet → Centro de redes y recursos compartidos → Cambiar configuración del adaptador** → clic derecho en tu adaptador de red activo → **Propiedades** → "Protocolo de Internet versión 4 (TCP/IPv4)" → **Propiedades** → "Usar las siguientes direcciones de servidor DNS":
   - DNS primario: `127.0.0.1`
   - DNS secundario: `8.8.8.8` (respaldo, opcional)
3. Limpia la caché de DNS de Windows (PowerShell o cmd como Administrador):
   ```
   ipconfig /flushdns
   ```
4. Prueba sin especificar servidor (debería resolver a través de tu `dns_server`):
   ```
   nslookup google.com
   ```
5. **Importante — revierte el cambio al terminar:** vuelve a poner "Obtener la dirección del servidor DNS automáticamente" en las propiedades del adaptador. Si no lo haces, te quedas sin resolución DNS en cuanto cierres el servidor o la ventana de Debian.

---

## Dónde está cada cosa en `lab_3.c` (y qué hace)

Todo vive en un solo archivo, dividido por comentarios `/* ---------- Seccion ---------- */`. De arriba a abajo:

| Sección | Líneas aprox. | Qué hace |
|---|---|---|
| **Config y constantes** | 32-64 | Puerto por defecto (53), IPs de los DNS públicos (`8.8.8.8` / `1.1.1.1`), tamaño de buffer, tamaño del thread pool, códigos de tipo de registro (`T_A`, `T_AAAA`, `T_CNAME`, `T_SOA`, `T_MX`, `T_PTR`, `T_NS`, etc.) y de `RCODE` (`NOERROR`, `SERVFAIL`, `NXDOMAIN`). |
| **Header DNS** | 66-128 | `parse_header()` / `write_header()`: leen y escriben los 12 bytes fijos de cualquier mensaje DNS (id, flags QR/AA/TC/RD/RA, contadores de cada sección) byte a byte, sin bitfields, para que funcione igual en Windows/Linux/ARM (BeagleBone). |
| **Nombres DNS** | 130-210 | `read_name()`: lee un nombre tipo `www.github.com`, siguiendo punteros de compresión si los hay (protegido contra loops maliciosos con un contador `guard`). `write_name_labels()` / `write_name_compressed()`: lo contrario, escribiendo con un puntero de compresión hacia el QNAME cuando el nombre coincide (ahorra espacio en la respuesta). |
| **EDNS0** | 212-259 | `query_edns_udp_size()`: revisa si el cliente anunció soporte EDNS0 (RR `OPT` en Additional) y qué tamaño de buffer UDP acepta; si no, aplica el límite clásico de 512 bytes del RFC 1035. |
| **Cache en memoria** | 261-441 | Lista enlazada global `g_cache` protegida por mutex. `cache_insert()` guarda un registro con su TTL y hora de inserción; `cache_lookup()` devuelve los vigentes que matchean (nombre, tipo, clase), descartando expirados; `cache_purge_rrset()` borra todo un RRset viejo antes de reemplazarlo por uno nuevo; hay un tope duro de `CACHE_MAX_ENTRIES` (4096) para que no crezca sin límite. |
| **`resolve_chain()`** (sigue CNAMEs) | 443-482 | Busca `(nombre, tipo)`; si no está pero el nombre tiene un `CNAME` cacheado, lo agrega a la respuesta y repite la búsqueda sobre el destino del alias (hasta 8 saltos). Así `www.github.com A` encuentra el `CNAME` guardado bajo el alias y el `A` guardado bajo `github.com`. |
| **Authority / Additional** | 484-520 | `lookup_up_the_tree()`: busca el `NS` o `SOA` subiendo por la jerarquía del nombre (`www.github.com` → `github.com` → `com`), para la sección Authority. `gather_glue()`: junta las direcciones `A`/`AAAA` de los nombres citados en Authority/Answer (destinos de NS, exchanges de MX) para la sección Additional. |
| **Parseo de RR** (`parse_rr`, `cache_response`) | 522-662 | Al recibir una respuesta del DNS público, recorre cada Resource Record y lo cachea según su tipo: `A`/`AAAA` como bytes crudos, `NS`/`CNAME`/`PTR` como nombre, `MX` como (preferencia + nombre), `SOA` con sus 5 campos numéricos completos. Lo demás (`TXT`, `SVCB`, `HTTPS`, `SRV`, etc.) se cachea como RDATA crudo tal cual, porque esos tipos no comprimen nombres adentro. Se hace en dos pasadas (`cache_response`) para no borrar hermanos del mismo RRset (ej. los 4 `NS` de un dominio). |
| **Construcción de la respuesta** (`write_rr`, `build_response`) | 664-816 | Arma el paquete de respuesta: header, Question, y las secciones Answer/Authority/Additional a partir de lo que se encontró en cache. Manda el **TTL restante** (no el original) para que el cliente no guarde el dato más tiempo del que realmente le queda de vida. Si la respuesta no cabe en el tamaño que el cliente acepta, trunca y marca `TC=1` en vez de mandar datos a medias. |
| **Reenvío recursivo** (`query_with_edns`, `forward_to_upstream`) | 818-879 | Manda la consulta a `8.8.8.8`, y si no responde en 2s, a `1.1.1.1` como respaldo. Antes de reenviar le agrega EDNS0 con nuestro tamaño de buffer completo (así el upstream no trunca RRsets grandes aunque el cliente original no use EDNS). |
| **Logging estructurado** (`log_line`) | 881-909 | Cada request/response se imprime en stdout (línea por línea, sin buffering) y se guarda en `dns_server.log`: timestamp, IP:puerto del cliente, `qname`, `qtype`, `action` (`CACHE_HIT` / `CACHE_MISS_FORWARD` / `SERVFAIL` / `MALFORMED`) y tiempo de respuesta en ms. |
| **Thread pool** | 932-978 | Cola circular de tareas (`g_queue`) con mutex + variables de condición. El hilo principal solo hace `recvfrom()` y encola; 8 hilos trabajadores (`worker_thread`) sacan de la cola y procesan cada consulta en paralelo. |
| **`handle_query()`** (el flujo completo) | 980-1083 | Parsea la consulta entrante → busca en cache con `resolve_chain()` → si está completa, `CACHE_HIT`; si no, reenvía al upstream, cachea la respuesta y reintenta → arma Authority/Additional → construye y envía la respuesta → registra el log. |
| **`main()`** | 1103-1180 | Lee puerto y DNS público de `argv`, crea el socket UDP, hace `bind()` (aquí falla con "Address already in use" si algo más ya tiene el puerto), levanta el thread pool, y en loop recibe paquetes y los encola hasta recibir `SIGINT`/`SIGTERM` (Ctrl+C), donde cierra todo ordenadamente. |

---

## 5. Limpiar

```bash
make clean       # borra el binario y dns_server.log
```

## Notas

- El código usa headers POSIX (`arpa/inet.h`, `pthread.h`, `unistd.h`): no compila nativamente en Windows, por eso todo esto se corre dentro de WSL/Debian.
- `dns_server.log` (en la carpeta del proyecto) tiene el historial completo de requests/respuestas, útil para adjuntar evidencia en la entrega.
- Para la entrega real en Debian/BeagleBone Black (sin WSL de por medio), los pasos 1 a 3 son los mismos — ahí no debería haber conflicto de puerto 53 con nada.
