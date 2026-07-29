/*
 * Laboratorio 3 - CC8: Servidor DNS recursivo con cache en memoria (UDP).
 *
 * Arquitectura:
 *   - Arranca con una tabla de cache VACIA (no persiste a disco, se pierde al reiniciar).
 *   - Si la consulta ya existe en cache (no expirada por TTL), la respuesta se
 *     construye desde ahi ("sale de tu laboratorio").
 *   - Si no existe, se reenvia UNA VEZ a un DNS publico (recursividad), se
 *     parsean TODOS los Resource Records de la respuesta, se guardan en cache,
 *     y la respuesta al cliente se reconstruye desde esos datos recien cacheados.
 *   - Multithreaded con un thread pool fijo de pthreads.
 *
 * Uso: ./dns_server [puerto=53] [ip_dns_publico=8.8.8.8]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ---------- Configuracion ---------- */

#define BUFFER_SIZE          4096
#define DEFAULT_PORT         53
#define DEFAULT_UPSTREAM_1   "8.8.8.8"   /* Google - hoja PROVEEDORES */
#define DEFAULT_UPSTREAM_2   "1.1.1.1"   /* Cloudflare - fallback */
#define UPSTREAM_PORT        53
#define UPSTREAM_TIMEOUT_SEC 2
#define THREAD_POOL_SIZE     8
#define QUEUE_CAP            64
#define MAX_NAME_LEN         256
#define RAW_RDATA_MAX        1024
#define CACHE_MAX_ENTRIES    4096
#define LOG_FILE             "dns_server.log"

/* Tipos de registro (hoja LISTA TIPO REGISTROS (QTYPE)) */
#define T_A      1
#define T_NS     2
#define T_CNAME  5
#define T_SOA    6
#define T_PTR    12
#define T_MX     15
#define T_TXT    16
#define T_AAAA   28
#define T_SRV    33
#define T_SVCB   64
#define T_HTTPS  65
#define T_OPT    41  /* pseudo-RR EDNS0, se ignora */

/* RCODEs */
#define RCODE_NOERROR  0
#define RCODE_SERVFAIL 2
#define RCODE_NXDOMAIN 3

/* ---------- Header DNS (RFC 1035 / hoja HEADER) ---------- */
/*
 * Se parsea/serializa a mano byte a byte (sin bitfields ni casts de
 * puntero a uint16_t/uint32_t) para evitar problemas de endianness y de
 * acceso desalineado en ARM (BeagleBone Black).
 */
typedef struct {
    uint16_t id;
    int qr, opcode, aa, tc, rd, ra, z, rcode;
    uint16_t qdcount, ancount, nscount, arcount;
} dns_header_t;

static uint16_t read_u16(const uint8_t *buf) {
    return (uint16_t)((buf[0] << 8) | buf[1]);
}

static uint32_t read_u32(const uint8_t *buf) {
    return ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
}

static void write_u16(uint8_t *buf, uint16_t v) {
    buf[0] = (uint8_t)((v >> 8) & 0xFF);
    buf[1] = (uint8_t)(v & 0xFF);
}

static void write_u32(uint8_t *buf, uint32_t v) {
    buf[0] = (uint8_t)((v >> 24) & 0xFF);
    buf[1] = (uint8_t)((v >> 16) & 0xFF);
    buf[2] = (uint8_t)((v >> 8) & 0xFF);
    buf[3] = (uint8_t)(v & 0xFF);
}

static void parse_header(const uint8_t *buf, dns_header_t *h) {
    h->id = read_u16(buf);
    uint8_t f1 = buf[2];
    uint8_t f2 = buf[3];
    h->qr     = (f1 >> 7) & 0x1;
    h->opcode = (f1 >> 3) & 0xF;
    h->aa     = (f1 >> 2) & 0x1;
    h->tc     = (f1 >> 1) & 0x1;
    h->rd     = f1 & 0x1;
    h->ra     = (f2 >> 7) & 0x1;
    h->z      = (f2 >> 4) & 0x7;
    h->rcode  = f2 & 0xF;
    h->qdcount = read_u16(buf + 4);
    h->ancount = read_u16(buf + 6);
    h->nscount = read_u16(buf + 8);
    h->arcount = read_u16(buf + 10);
}

static void write_header(uint8_t *buf, const dns_header_t *h) {
    write_u16(buf, h->id);
    uint8_t f1 = (uint8_t)((h->qr << 7) | ((h->opcode & 0xF) << 3) |
                            (h->aa << 2) | (h->tc << 1) | (h->rd & 0x1));
    uint8_t f2 = (uint8_t)((h->ra << 7) | ((h->z & 0x7) << 4) | (h->rcode & 0xF));
    buf[2] = f1;
    buf[3] = f2;
    write_u16(buf + 4, h->qdcount);
    write_u16(buf + 6, h->ancount);
    write_u16(buf + 8, h->nscount);
    write_u16(buf + 10, h->arcount);
}

/* ---------- Nombres DNS: lectura con descompresion, escritura con compresion ---------- */

/* Lee un nombre (posiblemente comprimido) en buf[pos..]. Escribe el resultado
 * en notacion "a.b.c" en out. Devuelve los bytes consumidos EN LA POSICION
 * ORIGINAL (sin contar lo que haya despues de seguir un puntero), o -1 en error. */
static int read_name(const uint8_t *buf, int buflen, int pos, char *out, size_t out_sz) {
    int start_pos = pos;
    int jumped = 0;
    int consumed = 0;
    size_t out_len = 0;
    int guard = 0;

    if (pos < 0 || pos >= buflen) return -1;
    out[0] = '\0';

    while (pos < buflen) {
        if (++guard > 128) return -1; /* evita loops por punteros maliciosos */
        uint8_t len = buf[pos];

        if (len == 0) {
            pos++;
            if (!jumped) consumed = pos - start_pos;
            break;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos + 1 >= buflen) return -1;
            int ptr = ((len & 0x3F) << 8) | buf[pos + 1];
            if (!jumped) consumed = (pos + 2) - start_pos;
            jumped = 1;
            if (ptr < 0 || ptr >= buflen) return -1;
            pos = ptr;
            continue;
        }
        pos++;
        if (pos + len > buflen) return -1;
        if (out_len + (size_t)len + 1 >= out_sz) return -1;
        if (out_len > 0) out[out_len++] = '.';
        memcpy(out + out_len, buf + pos, len);
        out_len += len;
        out[out_len] = '\0';
        pos += len;
    }
    return consumed;
}

/* Escribe "name" como labels DNS sin comprimir. Devuelve bytes escritos o -1. */
static int write_name_labels(const char *name, uint8_t *out, size_t out_cap) {
    size_t total = 0;
    if (name[0] == '\0') {
        if (out_cap < 1) return -1;
        out[0] = 0;
        return 1;
    }
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t label_len = dot ? (size_t)(dot - p) : strlen(p);
        if (label_len == 0 || label_len > 63) return -1;
        if (total + 1 + label_len + 1 > out_cap) return -1;
        out[total++] = (uint8_t)label_len;
        memcpy(out + total, p, label_len);
        total += label_len;
        if (!dot) break;
        p = dot + 1;
    }
    if (total + 1 > out_cap) return -1;
    out[total++] = 0;
    return (int)total;
}

/* Igual que write_name_labels, pero usa un puntero de compresion (0xC0 0x0C)
 * hacia el nombre de la Question cuando coincide exactamente. */
static int write_name_compressed(const char *name, const char *qname, uint8_t *out, size_t out_cap) {
    if (qname[0] && strcasecmp(name, qname) == 0) {
        if (out_cap < 2) return -1;
        out[0] = 0xC0;
        out[1] = 0x0C; /* offset 12: justo despues del header, donde vive el QNAME */
        return 2;
    }
    return write_name_labels(name, out, out_cap);
}

/* ---------- EDNS0: cuanto le cabe al cliente ---------- */

/* Salta un RR completo desde *pos, devolviendo su tipo y clase. */
static int skip_rr(const uint8_t *buf, int buflen, int *pos, uint16_t *type_out, uint16_t *class_out) {
    char tmp[MAX_NAME_LEN];
    int n = read_name(buf, buflen, *pos, tmp, sizeof(tmp));
    if (n < 0) return -1;
    *pos += n;
    if (*pos + 10 > buflen) return -1;
    if (type_out)  *type_out  = read_u16(buf + *pos);
    if (class_out) *class_out = read_u16(buf + *pos + 2);
    uint16_t rdlength = read_u16(buf + *pos + 8);
    *pos += 10 + rdlength;
    return (*pos > buflen) ? -1 : 0;
}

/* Tamano UDP que el cliente anuncia por EDNS0 (RR OPT en Additional, donde el
 * campo CLASS lleva el tamano). Devuelve 0 si la consulta no trae EDNS, en cuyo
 * caso aplica el limite clasico de 512 bytes del RFC 1035. */
static int query_edns_udp_size(const uint8_t *buf, int buflen) {
    if (buflen < 12) return 0;

    dns_header_t h;
    parse_header(buf, &h);
    if (h.arcount == 0) return 0;

    int pos = 12;
    for (int i = 0; i < h.qdcount; i++) {
        char tmp[MAX_NAME_LEN];
        int n = read_name(buf, buflen, pos, tmp, sizeof(tmp));
        if (n < 0) return 0;
        pos += n;
        if (pos + 4 > buflen) return 0;
        pos += 4;
    }

    int skip = h.ancount + h.nscount;
    for (int i = 0; i < skip; i++) {
        if (skip_rr(buf, buflen, &pos, NULL, NULL) < 0) return 0;
    }

    for (int i = 0; i < h.arcount; i++) {
        uint16_t type = 0, class_ = 0;
        if (skip_rr(buf, buflen, &pos, &type, &class_) < 0) return 0;
        if (type == T_OPT) return class_;
    }
    return 0;
}

/* ---------- Cache en memoria ---------- */

typedef enum { RD_RAW, RD_NAME, RD_SOA, RD_MX } rdata_kind_t;

typedef struct {
    char mname[MAX_NAME_LEN];
    char rname[MAX_NAME_LEN];
    uint32_t serial, refresh, retry, expire, minimum;
} soa_data_t;

typedef struct {
    uint16_t preference;
    char exchange[MAX_NAME_LEN];
} mx_data_t;

typedef struct cache_record {
    char name[MAX_NAME_LEN];
    uint16_t type;
    uint16_t class_;
    uint32_t ttl;
    time_t cached_at;
    rdata_kind_t kind;
    union {
        struct { uint8_t bytes[RAW_RDATA_MAX]; uint16_t len; } raw;
        char target[MAX_NAME_LEN];
        soa_data_t soa;
        mx_data_t mx;
    } data;
    struct cache_record *next;
} cache_record_t;

static cache_record_t *g_cache = NULL;
static int g_cache_count = 0;
static pthread_mutex_t g_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Segundos que lleva cacheada la entrada. Devuelve 0 si el reloj retrocedio,
 * para no convertir una resta negativa en un uint32 gigante ("todo expirado"). */
static uint32_t cache_age(const cache_record_t *r, time_t now) {
    double d = difftime(now, r->cached_at);
    if (d < 0) return 0;
    return (uint32_t)d;
}

static int cache_expired(const cache_record_t *r, time_t now) {
    return cache_age(r, now) >= r->ttl;
}

/* Compara el RDATA de dos entradas. Se usa para descartar duplicados exactos
 * sin borrar hermanos del mismo RRset (los 4 NS de google.com comparten
 * name/type/class pero tienen RDATA distinto, y los cuatro deben conservarse). */
static int rdata_equal(const cache_record_t *a, const cache_record_t *b) {
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case RD_RAW:
            return a->data.raw.len == b->data.raw.len &&
                   memcmp(a->data.raw.bytes, b->data.raw.bytes, a->data.raw.len) == 0;
        case RD_NAME:
            return strcasecmp(a->data.target, b->data.target) == 0;
        case RD_SOA:
            return a->data.soa.serial == b->data.soa.serial &&
                   strcasecmp(a->data.soa.mname, b->data.soa.mname) == 0 &&
                   strcasecmp(a->data.soa.rname, b->data.soa.rname) == 0;
        case RD_MX:
            return a->data.mx.preference == b->data.mx.preference &&
                   strcasecmp(a->data.mx.exchange, b->data.mx.exchange) == 0;
    }
    return 0;
}

static void cache_insert(const char *name, uint16_t type, uint16_t class_, uint32_t ttl,
                          rdata_kind_t kind, const void *data_ptr, size_t data_size) {
    cache_record_t *r = calloc(1, sizeof(*r));
    if (!r) return;

    strncpy(r->name, name, sizeof(r->name) - 1);
    r->type = type;
    r->class_ = class_;
    r->ttl = (ttl == 0) ? 1 : ttl; /* evita TTL=0 -> "siempre expirado" */
    r->cached_at = time(NULL);
    r->kind = kind;

    switch (kind) {
        case RD_RAW: {
            size_t copy_len = data_size;
            if (copy_len > sizeof(r->data.raw.bytes)) copy_len = sizeof(r->data.raw.bytes);
            memcpy(r->data.raw.bytes, data_ptr, copy_len);
            r->data.raw.len = (uint16_t)copy_len;
            break;
        }
        case RD_NAME:
            strncpy(r->data.target, (const char *)data_ptr, sizeof(r->data.target) - 1);
            break;
        case RD_SOA:
            memcpy(&r->data.soa, data_ptr, sizeof(soa_data_t));
            break;
        case RD_MX:
            memcpy(&r->data.mx, data_ptr, sizeof(mx_data_t));
            break;
    }

    pthread_mutex_lock(&g_cache_mutex);

    /* Una sola pasada: libera lo expirado y el duplicado exacto de esta misma
     * entrada (sin esto la lista solo crecia, y repetir una consulta que no se
     * cachea -como un NXDOMAIN, que reenvia siempre- acumulaba copias del SOA
     * de autoridad que luego salian repetidas en la respuesta). */
    cache_record_t **pp = &g_cache;
    while (*pp) {
        cache_record_t *cur = *pp;
        int dup = cur->type == r->type && cur->class_ == r->class_ &&
                  strcasecmp(cur->name, r->name) == 0 && rdata_equal(cur, r);
        if (dup || cache_expired(cur, r->cached_at)) {
            *pp = cur->next;
            free(cur);
            g_cache_count--;
        } else {
            pp = &cur->next;
        }
    }

    r->next = g_cache;
    g_cache = r;
    g_cache_count++;

    /* Tope duro: si aun quedan demasiadas vigentes, se descarta la mas antigua
     * (la cola de la lista) para que la memoria no crezca sin limite. */
    while (g_cache_count > CACHE_MAX_ENTRIES) {
        cache_record_t **tail = &g_cache;
        while ((*tail)->next) tail = &(*tail)->next;
        free(*tail);
        *tail = NULL;
        g_cache_count--;
    }

    pthread_mutex_unlock(&g_cache_mutex);
}

/* Borra el RRset completo de (name,type,class) y, de paso, todo lo expirado.
 * Se llama antes de insertar los RR de una respuesta nueva: en DNS un RRset
 * recibido REEMPLAZA al cacheado, no se suma a el. Sin esto quedaban versiones
 * viejas conviviendo con las nuevas (visible con el SOA de .com, cuyo serial
 * cambia cada pocos segundos, asi que ni siquiera son duplicados exactos). */
static void cache_purge_rrset(const char *name, uint16_t type, uint16_t class_) {
    time_t now = time(NULL);

    pthread_mutex_lock(&g_cache_mutex);
    cache_record_t **pp = &g_cache;
    while (*pp) {
        cache_record_t *cur = *pp;
        int same_rrset = cur->type == type && cur->class_ == class_ &&
                         strcasecmp(cur->name, name) == 0;
        if (same_rrset || cache_expired(cur, now)) {
            *pp = cur->next;
            free(cur);
            g_cache_count--;
        } else {
            pp = &cur->next;
        }
    }
    pthread_mutex_unlock(&g_cache_mutex);
}

/* Copia (bajo lock) las entradas de cache vigentes que matchean name/type/class. */
static int cache_lookup(const char *name, uint16_t type, uint16_t class_,
                         cache_record_t *out, int max_out) {
    int count = 0;
    time_t now = time(NULL);

    pthread_mutex_lock(&g_cache_mutex);
    for (cache_record_t *r = g_cache; r != NULL; r = r->next) {
        if (cache_expired(r, now)) continue;
        if (r->class_ != class_) continue;
        if (r->type != type) continue;
        if (strcasecmp(r->name, name) != 0) continue;
        if (count < max_out) out[count] = *r;
        count++;
    }
    pthread_mutex_unlock(&g_cache_mutex);

    return (count > max_out) ? max_out : count;
}

/* Resuelve (name, type) siguiendo la cadena de CNAMEs.
 *
 * Los RR se cachean bajo el nombre de su DUENO, asi que para un alias como
 * "www.github.com" el CNAME vive bajo el alias pero el registro A vive bajo
 * "github.com". Una busqueda directa por (alias, A) no encuentra nada; hay que
 * agregar el CNAME a la respuesta y repetir la busqueda sobre su destino.
 *
 * Devuelve cuantos RR quedaron en out (CNAMEs primero, luego los del tipo
 * pedido) y pone *complete a 1 solo si se llego a registros del tipo pedido. */
#define MAX_CNAME_DEPTH 8

static int resolve_chain(const char *name, uint16_t type, uint16_t class_,
                          cache_record_t *out, int max_out, int *complete) {
    char current[MAX_NAME_LEN];
    int count = 0;

    *complete = 0;
    strncpy(current, name, sizeof(current) - 1);
    current[sizeof(current) - 1] = '\0';

    for (int depth = 0; depth < MAX_CNAME_DEPTH && count < max_out; depth++) {
        int n = cache_lookup(current, type, class_, out + count, max_out - count);
        if (n > 0) {
            count += n;
            *complete = 1;
            break;
        }
        if (type == T_CNAME) break; /* el CNAME era el tipo pedido: no hay cadena que seguir */

        cache_record_t cn;
        if (cache_lookup(current, T_CNAME, class_, &cn, 1) < 1) break;
        if (cn.kind != RD_NAME) break; /* CNAME guardado como RDATA crudo: no se puede seguir */

        out[count++] = cn;
        strncpy(current, cn.data.target, sizeof(current) - 1);
        current[sizeof(current) - 1] = '\0';
    }

    return count;
}

/* ---------- Secciones Authority y Additional ---------- */

/* Busca (name,type) subiendo por la jerarquia: www.github.com -> github.com ->
 * com. Sirve para hallar el NS o el SOA de la zona que cubre al nombre
 * consultado, que es lo que va en la seccion Authority. */
static int lookup_up_the_tree(const char *name, uint16_t type, uint16_t class_,
                               cache_record_t *out, int max_out) {
    const char *p = name;
    for (int guard = 0; guard < 16 && *p; guard++) {
        int n = cache_lookup(p, type, class_, out, max_out);
        if (n > 0) return n;
        const char *dot = strchr(p, '.');
        if (!dot) break;
        p = dot + 1;
    }
    return 0;
}

/* Reune el "glue" para la seccion Additional: las direcciones A/AAAA de los
 * nombres a los que apuntan los RR de src (destinos NS y exchanges MX). */
static int gather_glue(const cache_record_t *src, int src_count, uint16_t class_,
                        cache_record_t *out, int max_out) {
    int count = 0;

    for (int i = 0; i < src_count && count < max_out; i++) {
        const char *target = NULL;
        if (src[i].type == T_NS && src[i].kind == RD_NAME) target = src[i].data.target;
        else if (src[i].type == T_MX && src[i].kind == RD_MX) target = src[i].data.mx.exchange;
        if (!target || !target[0]) continue;

        count += cache_lookup(target, T_A, class_, out + count, max_out - count);
        if (count < max_out) {
            count += cache_lookup(target, T_AAAA, class_, out + count, max_out - count);
        }
    }
    return count;
}

/* ---------- Parseo de Resource Records (usado para cachear la respuesta upstream) ---------- */

/* Modos de pasada sobre los RR de una respuesta upstream. */
#define RR_PASS_PURGE  1  /* solo invalida el RRset viejo de cada RR */
#define RR_PASS_INSERT 2  /* parsea el RDATA y cachea el RR */

/* Lee un RR completo en *pos (avanza *pos) y lo procesa segun el modo.
 * Devuelve 0 en exito, -1 si el paquete esta corrupto/truncado. */
static int parse_rr(const uint8_t *buf, int buflen, int *pos, int mode) {
    int store = (mode == RR_PASS_INSERT);
    char name[MAX_NAME_LEN];
    int n = read_name(buf, buflen, *pos, name, sizeof(name));
    if (n < 0) return -1;
    *pos += n;

    if (*pos + 10 > buflen) return -1; /* type+class+ttl+rdlength */
    uint16_t type = read_u16(buf + *pos); *pos += 2;
    uint16_t class_ = read_u16(buf + *pos); *pos += 2;
    uint32_t ttl = read_u32(buf + *pos); *pos += 4;
    uint16_t rdlength = read_u16(buf + *pos); *pos += 2;

    if (*pos + rdlength > buflen) return -1;
    int rdata_start = *pos;

    if (type == T_OPT) {
        *pos = rdata_start + rdlength; /* pseudo-RR EDNS0: se ignora, no se cachea */
        return 0;
    }

    if (mode == RR_PASS_PURGE) {
        cache_purge_rrset(name, type, class_);
        *pos = rdata_start + rdlength;
        return 0;
    }

    switch (type) {
        case T_A:
        case T_AAAA: {
            if (store) cache_insert(name, type, class_, ttl, RD_RAW, buf + rdata_start, rdlength);
            break;
        }
        case T_NS:
        case T_CNAME:
        case T_PTR: {
            char target[MAX_NAME_LEN];
            int tn = read_name(buf, buflen, rdata_start, target, sizeof(target));
            if (tn < 0) {
                if (store) cache_insert(name, type, class_, ttl, RD_RAW, buf + rdata_start, rdlength);
            } else if (store) {
                cache_insert(name, type, class_, ttl, RD_NAME, target, 0);
            }
            break;
        }
        case T_MX: {
            if (rdlength >= 2) {
                mx_data_t mx;
                memset(&mx, 0, sizeof(mx));
                mx.preference = read_u16(buf + rdata_start);
                int tn = read_name(buf, buflen, rdata_start + 2, mx.exchange, sizeof(mx.exchange));
                if (tn < 0) {
                    if (store) cache_insert(name, type, class_, ttl, RD_RAW, buf + rdata_start, rdlength);
                } else if (store) {
                    cache_insert(name, type, class_, ttl, RD_MX, &mx, sizeof(mx));
                }
            }
            break;
        }
        case T_SOA: {
            soa_data_t soa;
            memset(&soa, 0, sizeof(soa));
            int p2 = rdata_start;
            int mn = read_name(buf, buflen, p2, soa.mname, sizeof(soa.mname));
            int rn = -1;
            if (mn >= 0) {
                p2 += mn;
                rn = read_name(buf, buflen, p2, soa.rname, sizeof(soa.rname));
            }
            if (mn < 0 || rn < 0) {
                if (store) cache_insert(name, type, class_, ttl, RD_RAW, buf + rdata_start, rdlength);
                break;
            }
            p2 += rn;
            if (p2 + 20 > buflen) {
                if (store) cache_insert(name, type, class_, ttl, RD_RAW, buf + rdata_start, rdlength);
                break;
            }
            soa.serial  = read_u32(buf + p2); p2 += 4;
            soa.refresh = read_u32(buf + p2); p2 += 4;
            soa.retry   = read_u32(buf + p2); p2 += 4;
            soa.expire  = read_u32(buf + p2); p2 += 4;
            soa.minimum = read_u32(buf + p2); p2 += 4;
            if (store) cache_insert(name, type, class_, ttl, RD_SOA, &soa, sizeof(soa));
            break;
        }
        default: {
            /* TXT, SVCB, HTTPS, SRV, CAA, etc: se cachea el RDATA crudo tal
             * cual. Es correcto porque estos tipos no usan (o directamente
             * prohiben, como SVCB/HTTPS) compresion de nombres dentro de su
             * RDATA. */
            if (store && rdlength > 0) cache_insert(name, type, class_, ttl, RD_RAW, buf + rdata_start, rdlength);
            break;
        }
    }

    *pos = rdata_start + rdlength;
    return 0;
}

/* Recorre Answer+Authority+Additional de una respuesta upstream una vez. */
static void cache_response_pass(const uint8_t *buf, int buflen, int mode) {
    dns_header_t h;
    parse_header(buf, &h);

    int pos = 12;
    for (int i = 0; i < h.qdcount; i++) {
        char tmp[MAX_NAME_LEN];
        int n = read_name(buf, buflen, pos, tmp, sizeof(tmp));
        if (n < 0) return;
        pos += n;
        if (pos + 4 > buflen) return;
        pos += 4; /* qtype + qclass */
    }

    int total_rr = h.ancount + h.nscount + h.arcount;
    for (int i = 0; i < total_rr; i++) {
        if (parse_rr(buf, buflen, &pos, mode) < 0) break;
    }
}

/* Cachea una respuesta upstream en dos pasadas: primero invalida los RRsets
 * viejos que la respuesta trae, y despues inserta los RR nuevos.
 *
 * Tiene que ser en dos pasadas: purgar e insertar RR por RR haria que los
 * hermanos de un mismo RRset se borraran entre si (al llegar el 2do NS de
 * google.com se eliminaria el 1ro, y solo sobreviviria el ultimo). Purgar la
 * misma clave dos veces en la 1ra pasada es inofensivo: aun no se inserto nada. */
static void cache_response(const uint8_t *buf, int buflen) {
    if (buflen < 12) return;
    cache_response_pass(buf, buflen, RR_PASS_PURGE);
    cache_response_pass(buf, buflen, RR_PASS_INSERT);
}

/* ---------- Construccion de la respuesta hacia el cliente ---------- */

static int write_rr(uint8_t *buf, size_t cap, int pos, const cache_record_t *r, const char *qname) {
    int n = write_name_compressed(r->name, qname, buf + pos, cap - (size_t)pos);
    if (n < 0) return -1;
    pos += n;

    if ((size_t)(pos + 8) > cap) return -1;
    write_u16(buf + pos, r->type); pos += 2;
    write_u16(buf + pos, r->class_); pos += 2;

    /* Se envia el TTL RESTANTE, no el original: si se mandara siempre el valor
     * completo, el cliente creeria que el dato acaba de nacer y lo guardaria
     * mas tiempo del que le queda de vida real aqui. */
    uint32_t age = cache_age(r, time(NULL));
    uint32_t ttl_left = (age >= r->ttl) ? 1 : (r->ttl - age);
    write_u32(buf + pos, ttl_left); pos += 4;

    int rdlen_pos = pos;
    pos += 2;
    int rdata_start = pos;

    switch (r->kind) {
        case RD_RAW:
            if ((size_t)(pos + r->data.raw.len) > cap) return -1;
            memcpy(buf + pos, r->data.raw.bytes, r->data.raw.len);
            pos += r->data.raw.len;
            break;
        case RD_NAME: {
            int tn = write_name_compressed(r->data.target, qname, buf + pos, cap - (size_t)pos);
            if (tn < 0) return -1;
            pos += tn;
            break;
        }
        case RD_SOA: {
            int tn = write_name_labels(r->data.soa.mname, buf + pos, cap - (size_t)pos);
            if (tn < 0) return -1;
            pos += tn;
            int tn2 = write_name_labels(r->data.soa.rname, buf + pos, cap - (size_t)pos);
            if (tn2 < 0) return -1;
            pos += tn2;
            if ((size_t)(pos + 20) > cap) return -1;
            write_u32(buf + pos, r->data.soa.serial); pos += 4;
            write_u32(buf + pos, r->data.soa.refresh); pos += 4;
            write_u32(buf + pos, r->data.soa.retry); pos += 4;
            write_u32(buf + pos, r->data.soa.expire); pos += 4;
            write_u32(buf + pos, r->data.soa.minimum); pos += 4;
            break;
        }
        case RD_MX: {
            if ((size_t)(pos + 2) > cap) return -1;
            write_u16(buf + pos, r->data.mx.preference); pos += 2;
            int tn = write_name_compressed(r->data.mx.exchange, qname, buf + pos, cap - (size_t)pos);
            if (tn < 0) return -1;
            pos += tn;
            break;
        }
    }

    write_u16(buf + rdlen_pos, (uint16_t)(pos - rdata_start));
    return pos;
}

/* RR OPT (EDNS0) para la seccion Additional: nombre raiz, tipo 41, y en el
 * campo CLASS el tamano UDP que NOSOTROS aceptamos. Se devuelve solo si el
 * cliente uso EDNS, como hace cualquier resolver que anuncia soporte. */
static int write_opt_rr(uint8_t *buf, size_t cap, int pos) {
    if ((size_t)(pos + 11) > cap) return -1;
    buf[pos++] = 0;                                  /* nombre raiz */
    write_u16(buf + pos, T_OPT); pos += 2;
    write_u16(buf + pos, BUFFER_SIZE); pos += 2;     /* class = payload size */
    write_u32(buf + pos, 0); pos += 4;               /* ext-rcode, version, flags */
    write_u16(buf + pos, 0); pos += 2;               /* rdlength */
    return pos;
}

/* Escribe una seccion completa. Si un RR no cabe dentro de limit, deja la
 * posicion como estaba antes de ese RR y devuelve cuantos si entraron. */
static int write_section(uint8_t *resp, size_t limit, int *pos,
                          const cache_record_t *rrs, int count, const char *qname) {
    int written = 0;
    for (int i = 0; i < count; i++) {
        int np = write_rr(resp, limit, *pos, &rrs[i], qname);
        if (np < 0) break;
        *pos = np;
        written++;
    }
    return written;
}

static int build_response(uint8_t *resp, size_t cap,
                           uint16_t query_id, int rd,
                           const char *qname, uint16_t qtype, uint16_t qclass,
                           const cache_record_t *answers, int an_count,
                           const cache_record_t *authority, int ns_count,
                           const cache_record_t *additional, int ar_count,
                           int rcode, int edns_udp) {
    if (cap < 12) return -1;

    /* Cuanto puede recibir el cliente: lo que anuncio por EDNS0, o los 512
     * bytes del RFC 1035 si no uso EDNS. Nunca mas que nuestro buffer. */
    size_t limit = (edns_udp > 0) ? (size_t)edns_udp : 512;
    if (limit > cap) limit = cap;
    if (limit < 12) limit = 12;

    dns_header_t h;
    memset(&h, 0, sizeof(h));
    h.id = query_id;
    h.qr = 1;
    h.opcode = 0;
    h.aa = 0;
    h.tc = 0;
    h.rd = rd;
    h.ra = 1;
    h.z = 0;
    h.rcode = rcode;
    h.qdcount = 1;

    int pos = 12;
    int qn = write_name_labels(qname, resp + pos, limit - (size_t)pos);
    if (qn < 0) return -1;
    pos += qn;
    if ((size_t)(pos + 4) > limit) return -1;
    write_u16(resp + pos, qtype); pos += 2;
    write_u16(resp + pos, qclass); pos += 2;
    int end_of_question = pos;

    h.ancount = (uint16_t)write_section(resp, limit, &pos, answers, an_count, qname);

    /* Si la respuesta no cabe completa, se trunca a Question y se marca TC=1
     * para que el cliente reintente por TCP, en vez de mandar datos a medias. */
    if (h.ancount < an_count) {
        h.tc = 1;
        h.ancount = 0;
        pos = end_of_question;
    } else {
        /* Authority y Additional son informativas: si no caben se omiten sin
         * marcar TC (es lo que hacen los resolvers reales con el glue). */
        h.nscount = (uint16_t)write_section(resp, limit, &pos, authority, ns_count, qname);
        h.arcount = (uint16_t)write_section(resp, limit, &pos, additional, ar_count, qname);
    }

    if (edns_udp > 0) {
        int np = write_opt_rr(resp, limit, pos);
        if (np > 0) {
            pos = np;
            h.arcount++;
        }
    }

    write_header(resp, &h);
    return pos;
}

/* ---------- Reenvio recursivo a un DNS publico ---------- */

static char g_upstream_1[64] = DEFAULT_UPSTREAM_1;
static char g_upstream_2[64] = DEFAULT_UPSTREAM_2;

/* Copia la consulta agregandole EDNS0 si no lo trae, anunciando nuestro buffer.
 *
 * Es necesario porque la consulta al upstream NO debe heredar los limites del
 * cliente: si un cliente sin EDNS pregunta por un RRset grande (p.ej. los TXT
 * de google.com, ~1KB), reenviar su consulta tal cual hace que el upstream
 * responda truncado a 512 bytes y sin registros, y entonces no habria nada que
 * cachear. Se pide completo hacia afuera y ya se recorta al responderle a el. */
static int query_with_edns(const uint8_t *query, int qlen, uint8_t *out, size_t out_cap) {
    if ((size_t)qlen > out_cap) return -1;
    memcpy(out, query, (size_t)qlen);

    if (query_edns_udp_size(query, qlen) > 0) return qlen; /* ya trae OPT */
    if ((size_t)(qlen + 11) > out_cap) return qlen;

    int pos = write_opt_rr(out, out_cap, qlen);
    if (pos < 0) return qlen;

    dns_header_t h;
    parse_header(out, &h);
    h.arcount++;
    write_header(out, &h);
    return pos;
}

static int forward_to_upstream(const uint8_t *query, int qlen, uint8_t *out, size_t out_cap) {
    const char *servers[2] = { g_upstream_1, g_upstream_2 };

    for (int i = 0; i < 2; i++) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) continue;

        struct timeval tv;
        tv.tv_sec = UPSTREAM_TIMEOUT_SEC;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in up;
        memset(&up, 0, sizeof(up));
        up.sin_family = AF_INET;
        up.sin_port = htons(UPSTREAM_PORT);
        if (inet_pton(AF_INET, servers[i], &up.sin_addr) != 1) {
            close(sock);
            continue;
        }

        if (sendto(sock, query, qlen, 0, (struct sockaddr *)&up, sizeof(up)) < 0) {
            close(sock);
            continue;
        }

        ssize_t n = recvfrom(sock, out, out_cap, 0, NULL, NULL);
        close(sock);
        if (n > 0) return (int)n;
        /* timeout o error: intenta el siguiente servidor de la lista */
    }
    return -1;
}

/* ---------- Logging estructurado ---------- */

static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

static void log_line(const char *client_ip, int client_port, const char *qname,
                      const char *qtype, const char *action, const char *fmt, ...) {
    char extra[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tsbuf[32];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tmv);

    pthread_mutex_lock(&g_log_mutex);
    FILE *fp = fopen(LOG_FILE, "a");
    fprintf(stdout, "[%s] client=%s:%d qname=%s qtype=%s action=%s %s\n",
            tsbuf, client_ip, client_port, qname, qtype, action, extra);
    if (fp) {
        fprintf(fp, "[%s] client=%s:%d qname=%s qtype=%s action=%s %s\n",
                tsbuf, client_ip, client_port, qname, qtype, action, extra);
        fclose(fp);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

static void qtype_to_str(uint16_t t, char *out, size_t cap) {
    const char *s;
    switch (t) {
        case T_A: s = "A"; break;
        case T_NS: s = "NS"; break;
        case T_CNAME: s = "CNAME"; break;
        case T_SOA: s = "SOA"; break;
        case T_PTR: s = "PTR"; break;
        case T_MX: s = "MX"; break;
        case T_TXT: s = "TXT"; break;
        case T_AAAA: s = "AAAA"; break;
        case T_SRV: s = "SRV"; break;
        case T_SVCB: s = "SVCB"; break;
        case T_HTTPS: s = "HTTPS"; break;
        default:
            snprintf(out, cap, "TYPE%u", (unsigned)t);
            return;
    }
    snprintf(out, cap, "%s", s);
}

/* ---------- Thread pool ---------- */

typedef struct {
    int sockfd;
    struct sockaddr_in client_addr;
    socklen_t addrlen;
    uint8_t buf[BUFFER_SIZE];
    int len;
} task_t;

static task_t g_queue[QUEUE_CAP];
static int g_qhead = 0, g_qtail = 0, g_qcount = 0;
static pthread_mutex_t g_qmutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_qnotempty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_qnotfull = PTHREAD_COND_INITIALIZER;
static volatile sig_atomic_t g_running = 1;

static void queue_push(const task_t *t) {
    pthread_mutex_lock(&g_qmutex);
    while (g_qcount == QUEUE_CAP && g_running) {
        pthread_cond_wait(&g_qnotfull, &g_qmutex);
    }
    if (g_running) {
        g_queue[g_qtail] = *t;
        g_qtail = (g_qtail + 1) % QUEUE_CAP;
        g_qcount++;
        pthread_cond_signal(&g_qnotempty);
    }
    pthread_mutex_unlock(&g_qmutex);
}

static int queue_pop(task_t *t) {
    pthread_mutex_lock(&g_qmutex);
    while (g_qcount == 0 && g_running) {
        pthread_cond_wait(&g_qnotempty, &g_qmutex);
    }
    if (g_qcount == 0) {
        pthread_mutex_unlock(&g_qmutex);
        return 0;
    }
    *t = g_queue[g_qhead];
    g_qhead = (g_qhead + 1) % QUEUE_CAP;
    g_qcount--;
    pthread_cond_signal(&g_qnotfull);
    pthread_mutex_unlock(&g_qmutex);
    return 1;
}

/* ---------- Manejo de una consulta ---------- */

static void handle_query(task_t *t) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &t->client_addr.sin_addr, client_ip, sizeof(client_ip));
    int client_port = ntohs(t->client_addr.sin_port);

    if (t->len < 12) return; /* demasiado corto para ser DNS: se descarta en silencio */

    dns_header_t qh;
    parse_header(t->buf, &qh);
    if (qh.qdcount < 1) return;

    int pos = 12;
    char qname[MAX_NAME_LEN];
    int nlen = read_name(t->buf, t->len, pos, qname, sizeof(qname));
    if (nlen < 0) {
        log_line(client_ip, client_port, "?", "?", "MALFORMED", "no se pudo parsear QNAME");
        return;
    }
    pos += nlen;
    if (pos + 4 > t->len) {
        log_line(client_ip, client_port, qname, "?", "MALFORMED", "paquete truncado");
        return;
    }
    uint16_t qtype = read_u16(t->buf + pos); pos += 2;
    uint16_t qclass = read_u16(t->buf + pos); pos += 2;

    char qtype_str[16];
    qtype_to_str(qtype, qtype_str, sizeof(qtype_str));

    int edns_udp = query_edns_udp_size(t->buf, t->len);

    cache_record_t matches[32];
    int complete = 0;
    int mcount = resolve_chain(qname, qtype, qclass, matches, 32, &complete);

    uint8_t resp[BUFFER_SIZE];
    int resp_len;
    const char *action;

    /* Solo cuenta como hit si la cadena llego hasta registros del tipo pedido;
     * un CNAME suelto sin su destino se reenvia para completarlo. */
    int rcode = RCODE_NOERROR;
    if (complete) {
        action = "CACHE_HIT";
    } else {
        uint8_t upbuf[BUFFER_SIZE];
        uint8_t upquery[BUFFER_SIZE];
        int uqlen = query_with_edns(t->buf, t->len, upquery, sizeof(upquery));
        if (uqlen < 0) { uqlen = t->len; memcpy(upquery, t->buf, (size_t)t->len); }
        int uplen = forward_to_upstream(upquery, uqlen, upbuf, sizeof(upbuf));
        if (uplen > 0) {
            cache_response(upbuf, uplen);
            mcount = resolve_chain(qname, qtype, qclass, matches, 32, &complete);
            if (mcount == 0) {
                dns_header_t uph;
                parse_header(upbuf, &uph);
                rcode = uph.rcode;
            }
            action = "CACHE_MISS_FORWARD";
        } else {
            mcount = 0;
            rcode = RCODE_SERVFAIL;
            action = "SERVFAIL";
        }
    }

    /* Authority: si hay respuesta, los NS de la zona que cubre al nombre; si no
     * la hay (NXDOMAIN / sin datos), el SOA de la zona, igual que un resolver
     * real. Se omite cuando el propio qtype ya devolvio esos RR en Answer. */
    cache_record_t authority[8], additional[16];
    int ns_count = 0, ar_count = 0;

    if (mcount > 0) {
        if (qtype != T_NS && qtype != T_SOA) {
            ns_count = lookup_up_the_tree(qname, T_NS, qclass, authority, 8);
        }
    } else {
        ns_count = lookup_up_the_tree(qname, T_SOA, qclass, authority, 1);
    }

    /* Additional: direcciones de los nombres citados en Authority (NS) y en
     * Answer (exchanges MX), para ahorrarle al cliente esas consultas. */
    ar_count = gather_glue(authority, ns_count, qclass, additional, 16);
    if (ar_count < 16) {
        ar_count += gather_glue(matches, mcount, qclass, additional + ar_count, 16 - ar_count);
    }

    resp_len = build_response(resp, sizeof(resp), qh.id, qh.rd, qname, qtype, qclass,
                               matches, mcount, authority, ns_count, additional, ar_count,
                               rcode, edns_udp);

    if (resp_len > 0) {
        sendto(t->sockfd, resp, resp_len, 0, (struct sockaddr *)&t->client_addr, t->addrlen);
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    log_line(client_ip, client_port, qname, qtype_str, action, "%.2fms", ms);
}

static void *worker_thread(void *arg) {
    (void)arg;
    task_t t;
    while (queue_pop(&t)) {
        handle_query(&t);
    }
    return NULL;
}

/* ---------- Arranque / cierre ---------- */

static int g_listen_sock = -1;

static void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
}

int main(int argc, char *argv[]) {
    /* Linea a linea: sin esto, al redirigir la salida (make run > log.txt, | tee)
     * stdout queda con buffer por bloques y los logs no aparecen hasta que el
     * proceso termina, como si el servidor no estuviera registrando nada. */
    setvbuf(stdout, NULL, _IOLBF, 0);

    int port = DEFAULT_PORT;
    if (argc > 1) {
        int p = atoi(argv[1]);
        if (p > 0 && p <= 65535) port = p;
    }
    if (argc > 2) {
        strncpy(g_upstream_1, argv[2], sizeof(g_upstream_1) - 1);
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    g_listen_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_listen_sock < 0) {
        perror("[-] Error al crear el socket de escucha");
        return EXIT_FAILURE;
    }

    int opt = 1;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(g_listen_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in servaddr;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons((uint16_t)port);

    if (bind(g_listen_sock, (struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("[-] Error en bind (¿necesitas sudo para el puerto 53?)");
        close(g_listen_sock);
        return EXIT_FAILURE;
    }

    log_line("-", 0, "-", "-", "START", "puerto=%d upstream_primario=%s upstream_fallback=%s",
              port, g_upstream_1, g_upstream_2);

    pthread_t pool[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_create(&pool[i], NULL, worker_thread, NULL);
    }

    while (g_running) {
        task_t t;
        t.addrlen = sizeof(t.client_addr);
        ssize_t n = recvfrom(g_listen_sock, t.buf, BUFFER_SIZE, 0,
                              (struct sockaddr *)&t.client_addr, &t.addrlen);
        if (n < 0) {
            continue; /* timeout (EAGAIN) u otro error transitorio: reintenta y revisa g_running */
        }
        t.sockfd = g_listen_sock;
        t.len = (int)n;
        queue_push(&t);
    }

    printf("\n[+] Cerrando servidor DNS...\n");
    pthread_mutex_lock(&g_qmutex);
    pthread_cond_broadcast(&g_qnotempty);
    pthread_cond_broadcast(&g_qnotfull);
    pthread_mutex_unlock(&g_qmutex);

    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        pthread_join(pool[i], NULL);
    }

    close(g_listen_sock);
    return EXIT_SUCCESS;
}
