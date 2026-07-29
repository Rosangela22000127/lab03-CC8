
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

### Nota de plataforma

El código usa headers POSIX (`arpa/inet.h`, `pthread.h`, `unistd.h`) — compila y corre en Linux/Debian/BeagleBone, no de forma nativa en Windows (se necesita WSL o una VM Linux).

### Paso 0 (solo Windows): preparar WSL con Debian

Si desarrollas en Windows, necesitas un entorno Linux real. La opción más simple es WSL con Debian (mismo target que pide el bonus de BeagleBone):

```powershell
# En PowerShell como Administrador, una sola vez:
wsl --install -d Debian
# reinicia si te lo pide; al abrir la ventana de Debian, crea tu usuario/contraseña de Linux
```

Dentro de esa ventana de Debian, instala las herramientas de compilación (una sola vez):

```bash
sudo apt update && sudo apt install -y build-essential dnsutils
```

### Compilar

```bash
cd "/mnt/c/ruta/a/tu/Lab03"   # tu carpeta del repo, montada bajo /mnt/c/...
make
```

### Prueba rápida, sin privilegios (puerto 5353)

**Ventana 1** (Debian) — deja el servidor corriendo en primer plano:

```bash
./dns_server 5353
```

**Ventana 2** (abre otra ventana "Debian" desde el menú inicio) — manda consultas:

```bash
dig @127.0.0.1 -p 5353 www.github.com A     # 1ra vez: CACHE_MISS_FORWARD en el log (ventana 1). Responde CNAME + A
dig @127.0.0.1 -p 5353 www.github.com A     # repite: CACHE_HIT
dig @127.0.0.1 -p 5353 google.com AAAA
dig @127.0.0.1 -p 5353 google.com MX
dig @127.0.0.1 -p 5353 google.com NS
dig @127.0.0.1 -p 5353 google.com SOA
dig @127.0.0.1 -p 5353 -x 8.8.8.8            # PTR
```

`Ctrl+C` en la ventana 1 para parar el servidor. Revisa también `dns_server.log` para confirmar el formato de cada entrada y que no haya errores en consola.

### Prueba en el puerto real 53 (como lo pide el lab)

El puerto 53 es privilegiado y necesita `sudo`:

**Ventana 1:**

```bash
sudo ./dns_server        # usa el puerto 53 por defecto
```

**Ventana 2** (ya no hace falta `-p`, 53 es el default de `dig`):

```bash
dig @127.0.0.1 google.com A
```

### (Opcional/avanzado) Configurar tu equipo para usar 127.0.0.1 como DNS primario

Esto es lo que pide literalmente el enunciado ("configura tu equipo..."), pero cambia la configuración real de red de tu máquina — para el entregable normalmente basta con la prueba anterior (`dig` directo al puerto 53). Si igual quieres esta demo extra:

- **Windows:** Panel de Control → Redes e Internet → Centro de redes... → Cambiar configuración del adaptador → clic derecho en tu adaptador activo → Propiedades → "Protocolo de Internet versión 4 (TCP/IPv4)" → Propiedades → "Usar las siguientes direcciones de servidor DNS" → primario `127.0.0.1`, secundario `8.8.8.8`.
- Deja `sudo ./dns_server` corriendo en WSL mientras pruebas (WSL2 reenvía el puerto de `127.0.0.1` automáticamente hacia Windows).
- Limpia la caché de DNS: `ipconfig /flushdns` (cmd/PowerShell como administrador), luego `nslookup google.com` (sin especificar servidor) — debería resolver a través de tu servidor.
- **Importante:** revierte el DNS del adaptador a "Obtener la dirección del servidor DNS automáticamente" al terminar, o te quedas sin resolución DNS si el proceso no está corriendo.

---
