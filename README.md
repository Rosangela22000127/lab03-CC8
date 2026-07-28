
# Laboratorio 3 - Servidor DNS sobre UDP

Este Laboratorio Consiste en la implementación de un servidor DNS utilizando el protocolo UDP. El laboratorio contiene dos componentes principales:

- Código en **C** (`lab_3.c`) para manejar sockets UDP.
- Código en **Java** (`UdpBroadcastSender.java`) para enviar mensajes en modo broadcast sobre UDP.
- Un Archivo Makefile para agiliar la compilacion y manejo de archivos.

Este código es proporcionado para que lo utilice como ejemplo para inicar en la lectura del protocolo de DNS que utiliza su sistema operativo para comunicarse. 

---

## 🛠️ Makefile - Compilación y ejecución

El `Makefile` incluido facilita la compilación, ejecución y limpieza del entorno. A continuación, se describen los comandos disponibles:

### Cómo usar el Makefile

- **Compilar el programa en C:**
  ```bash
  make
  ```
  Genera el binario `udp_test`.

- **Ejecutar el binario compilado:**
  ```bash
  make run
  ```

- **Ejecutar el programa Java (broadcast sender):**
  ```bash
  make runjava
  ```

- **Liberar el puerto UDP 9999 si está ocupado:**
  ```bash
  make killudp
  ```
  Si no esta ocupado deberia lanzar un Error por no encontrar ninguna concidencia

- **Limpiar archivos compilados:**
  ```bash
  make clean
  ```

---

## Descripción del laboratorio

### Objetivo

Implementar un **Servidor DNS** en Java que escuche en el puerto `53` (UDP), sea capaz de interpretar correctamente consultas DNS, construir respuestas válidas, y manejar recursividad hacia servidores DNS públicos.

### 🔧 Requisitos principales

- Escuchar en `UDP:53`.
- Construir mensajes DNS respetando la estructura definida por el [RFC 5395](https://datatracker.ietf.org/doc/html/rfc5395).
- Soportar múltiples tipos de registros: `A`, `AAAA`, `PTR`, `SOA`, `SVCB`, `HTTPS`, etc.
- Implementar recursividad con reenvío hacia servidores DNS públicos.
- Registrar logs con cada request/response.
- Multithreading con `ThreadPool` (basado en el Lab #2). (en Java)
- Implementar al menos 3 tipos de registros de forma completa.

### 🧪 Configuración para pruebas

- Configura tu equipo para que use `127.0.0.1` como DNS primario. (Importante)
- Agrega un DNS externo secundario como respaldo (Google, Cloudflare, etc.). (Opcional)
- Limpia cachés DNS del sistema operativo antes de hacer pruebas: 

```bash
# Para Ubuntu 17.04 y superior (18.04)
sudo systemd-resolve --flush-caches

# Para Ubuntu 22.04 y superior
sudo resolvectl flush-caches
```

```bash
# Para MACOS
sudo dscacheutil -flushcache; sudo killall -HUP mDNSResponder"
```

```bash
# Para Windows
•  Asegúrate de que te encuentras en el escritorio de Windows 10 o Windows 11.
•  Haz clic derecho en el menú de inicio y elige ""Símbolo del sistema (Administrador)"" o ""Windows PowerShell (Administrador)"".
•  Introduce el comando ""ipconfig /flushdns"".
•  Pulsa la tecla Intro de tu teclado."

```

> Usa Wireshark para analizar paquetes en `udp.port == 53`.


### Descripcion de Laboratorio y Ejemplos en el siguiente enlace:

> https://docs.google.com/spreadsheets/d/1uCG9dcFXyNORk5xggtNzUZxyhIXZPvKW_mXAM1srDCE/edit?usp=sharing


### Entregable

- Archivos fuente `.java`, `.c`, `Makefile`,  Depende de su implementacion
- Archivo `README.md`.
- Video explicativo (si no presentas presencialmente).
- Compilación y ejecución en sistema Debian (preferentemente sobre BeagleBone Black para obtener hasta 150%).

---

## Requisitos técnicos

- Java versión 21 o 22.
- Código en C compilable con `gcc`. (si elige esta opción)
- Uso de `DatagramSocket` para manejo de UDP. (si usa JAVA)
- Logs estructurados.
- Manejo de errores sin fallos en consola.

---

## ⚠Penalizaciones para calificación Cero

- No usar laboratorios anteriores como base (DatagramSocket, ThreadPool, etc.). (si va por JAVA)
- Logs mal estructurados o inexistentes.
- Menos de 3 tipos de registros funcionales.
- Errores en consola del servidor o cliente.

---

## ⭐ Bonus

+50% si se implementa completamente en C, corriendo sobre Debian en BeagleBone Black con conexión Ethernet.

---

## Implementación (C)

`lab_3.c` implementa un **resolver DNS recursivo con caché en memoria** sobre UDP, no un proxy transparente:

- **Arranca con la caché vacía** (no se persiste a disco; se pierde al reiniciar el proceso, como exige el lab).
- **Cache hit:** si el `(nombre, tipo, clase)` ya está en la caché y no expiró (según su TTL), la respuesta se construye directamente ahí — "sale de tu laboratorio".
- **Cache miss:** se reenvía la consulta **una sola vez** a un DNS público (recursividad), se parsean **todos** los Resource Records de la respuesta (Answer/Authority/Additional, con descompresión de nombres), se guardan en la caché, y la respuesta al cliente se reconstruye desde esos datos recién cacheados. Si el upstream no contesta (timeout ~2s), responde `SERVFAIL` en vez de fallar.
- **Upstream por defecto:** Google `8.8.8.8` (fallback Cloudflare `1.1.1.1` si el primero no responde) — de la hoja PROVEEDORES. Se puede cambiar con `argv[2]`.
- **Tipos con parsing/reconstrucción estructural completa:** `A`, `AAAA`, `CNAME`, `NS`, `PTR`, `SOA`, `MX` (por encima del mínimo de 3 que pide el lab). El resto (`TXT`, `SVCB`, `HTTPS`, `SRV`, `CAA`, etc.) se cachea y reconstruye como RDATA crudo, lo cual es válido porque esos tipos no comprimen nombres dentro de su RDATA.
- **Multithreading:** thread pool fijo de 8 hilos (pthreads) + cola de tareas con mutex/cond; el hilo principal solo hace `recvfrom` y encola.
- **Logs estructurados:** cada request/response se registra en stdout y en `dns_server.log` con timestamp, IP:puerto del cliente, qname, qtype, acción (`CACHE_HIT` / `CACHE_MISS_FORWARD` / `SERVFAIL` / `MALFORMED`) y tiempo de respuesta.

### Cómo correr

```bash
make              # compila -> genera ./dns_server
sudo make run     # escucha en el puerto 53 (requiere privilegios)
make run PORT=5353  # o en un puerto sin privilegios, para pruebas
```

### Cómo probar

```bash
dig @127.0.0.1 -p 5353 google.com A      # 1ra vez: CACHE_MISS_FORWARD en el log
dig @127.0.0.1 -p 5353 google.com A      # 2da vez: CACHE_HIT
dig @127.0.0.1 -p 5353 google.com AAAA
dig @127.0.0.1 -p 5353 google.com MX
dig @127.0.0.1 -p 5353 google.com NS
dig @127.0.0.1 -p 5353 -x 8.8.8.8         # PTR
```

Revisa `dns_server.log` para confirmar el formato de cada entrada y que no haya errores en consola.

### Nota de plataforma

El código usa headers POSIX (`arpa/inet.h`, `pthread.h`, `unistd.h`) — compila y corre en Linux/Debian/BeagleBone, no de forma nativa en Windows (se necesita WSL o una VM Linux).

---
