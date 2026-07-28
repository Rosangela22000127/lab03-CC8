
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
