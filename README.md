
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

## Ejecución en BeagleBone Black (Debian) por Ethernet — bonus 150%

Verificado sobre `BeagleBoard.org Debian Buster IoT Image 2020-04-06`, kernel `4.19.94-ti-r42`,
arquitectura `armv7l`. Compila limpio con `-Wall -Wextra` (0 warnings) y corre con ~1.5 MB de RSS.

### Topología

Dos variantes; ambas cumplen el requisito de "Ethernet a un Router u otro Dispositivo":

```
A) Con router:            Mac ──router── BBB          (el BBB toma IP por DHCP del router)
B) Cable directo:  WiFi ── Mac ──NAT──> adaptador USB-Ethernet ──cable── BBB
```

La opción B es la de la presentación (sin router propio): la Mac comparte su internet
por el cable. **Ajustes → General → Compartir → Compartir Internet**, compartir desde
*Wi-Fi* hacia el adaptador *USB 10/100/1000 LAN*. macOS levanta DHCP + NAT y queda como
`192.168.2.1`, el BBB recibe `192.168.2.x`.

### 1. En la Mac: verificar el enlace

Después de conectar (o reconectar) el dongle, en una sola línea:

```bash
networksetup -listallhardwareports | grep -A1 "USB 10/100/1000 LAN" | grep Device; ifconfig | grep -q "inet 192.168.2.1" && echo "sharing OK" || echo "SHARING CAIDO"; grep -A1 name=beaglebone /var/db/dhcpd_leases | grep ip_address; ping -c2 -t3 192.168.2.2 >/dev/null && echo "ping OK" || echo "NO responde"
```

Salida esperada: `Device: en7` / `sharing OK` / `ip_address=192.168.2.2` / `ping OK`.

- `SHARING CAIDO` → al desconectar el dongle macOS apaga Compartir Internet: apágalo y préndelo.
- `Device:` distinto → si cambiaste de puerto USB-C, vuelve a marcar la casilla del adaptador nuevo.
- `ip_address=` distinto → usa esa IP en todo lo que sigue.

### 2. Copiar las fuentes y compilar en la placa

`gcc` y `make` ya vienen en la imagen IoT; no hay que instalar nada.

```bash
ssh debian@192.168.2.2 'mkdir -p ~/lab03'
scp lab_3.c Makefile debian@192.168.2.2:~/lab03/
ssh debian@192.168.2.2 'cd ~/lab03 && make'
```

### 3. Liberar el puerto 53 (una sola vez)

En estas imágenes `dnsmasq` ocupa el 53 (sirve la red por USB, que no se usa aquí). El
`disable` evita que regrese tras reiniciar:

```bash
ssh -t debian@192.168.2.2 "sudo systemctl stop dnsmasq && sudo systemctl disable dnsmasq"
```

No afecta la resolución del propio BBB: su `/etc/resolv.conf` apunta al router / a la Mac,
no a `127.0.0.1`.

### 4. Arrancar el servidor

En primer plano, para que los logs se vean en vivo durante la demo:

```bash
ssh debian@192.168.2.2
cd ~/lab03 && sudo ./dns_server 53
```

### 5. Probar desde la Mac (cruzando el Ethernet)

```bash
dig @192.168.2.2 google.com A      # 1ra vez: CACHE_MISS_FORWARD (~35 ms)
dig @192.168.2.2 google.com A      # 2da vez: CACHE_HIT (~1 ms)
dig @192.168.2.2 google.com AAAA
dig @192.168.2.2 google.com MX
dig @192.168.2.2 google.com NS
dig @192.168.2.2 google.com TXT
dig @192.168.2.2 github.com SOA
dig @192.168.2.2 -x 8.8.8.8                  # PTR
dig @192.168.2.2 www.github.com A            # cadena CNAME -> ANSWER: 2
dig @192.168.2.2 cloudflare.com TYPE65       # HTTPS
dig @192.168.2.2 _dns.resolver.arpa TYPE64   # SVCB
dig @192.168.2.2 noexiste-xyz.com A          # NXDOMAIN + SOA en Authority
```

En el log de la placa el cliente aparece como `192.168.2.1` (la Mac), que es la evidencia
de que las consultas cruzan la red y no salen de `localhost`.

Captura de paquetes en la placa, si no hay GUI para Wireshark:

```bash
sudo tcpdump -i eth0 -n udp port 53
```

### Notas de la plataforma

- **`dig` viejo y los tipos nuevos:** las versiones anteriores a BIND 9.17 no reconocen
  `HTTPS`/`SVCB` como tipo y lo interpretan como *nombre de host*, devolviendo registros A
  sin haber consultado nunca el tipo 64/65. Usa siempre `TYPE65` y `TYPE64`.
- **El reloj no tiene batería.** Al encender, el BBB arranca en `2000-01-01` hasta que NTP
  lo corrige. Si el servidor se levanta en esa ventana, **los logs quedan con fecha del año
  2000**. Antes de arrancarlo, confirma:
  ```bash
  timedatectl | grep -i synchron     # debe decir: System clock synchronized: yes
  ```
- **Zona horaria:** la imagen viene en UTC (6 horas adelante de Guatemala). Para que los
  logs coincidan con la hora local: `sudo timedatectl set-timezone America/Guatemala`.
- **Alimentación:** si el BBB se alimenta por USB desde el hub, desconectar el hub **reinicia
  la placa** (y con eso muere el servidor y se pierde la caché). Con un cargador de 5V al
  jack de barril queda independiente.
- **El servidor no arranca solo** tras un reinicio: hay que volver a levantarlo por SSH.
- **Cable FTDI:** sólo da consola serial (`screen /dev/cu.usbserial-XXXX 115200`), no
  interviene en la operación. Conviene llevarlo igual: es la única vía de acceso si la red falla.
- **Espacio en disco:** la imagen puede llenar el eMMC de 4 GB con logs rotados. Si `df -h /`
  marca 100%, `sudo rm -f /var/log/*.1 /var/log/*.gz` libera el espacio (son rotados, ya no
  los escribe nadie).

---



