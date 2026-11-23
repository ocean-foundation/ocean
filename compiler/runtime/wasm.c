/* Ocean runtime support for wasm32-unknown-unknown: the small, freestanding
 * subset of the C library that core.c uses. It is used both as the header of
 * the generated <stdlib.h>, <stdio.h>, ... shims (declarations only) and, with
 * OCEAN_WASM_IMPL defined, as their implementation.
 *
 * Linear memory grows on demand through memory.grow. The host may provide
 * env.ocean_write(fd, pointer, length) to receive panic reports; nothing is
 * imported unless such output is reachable. Without native unwinding there is
 * no setjmp/longjmp: recovering from a panic traps. */
#ifndef OCEAN_WASM_LIBC
#define OCEAN_WASM_LIBC
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

typedef long ssize_t;
typedef struct OcWasmFile { int fd; } FILE;
extern FILE *stdout, *stderr;
#define EOF (-1)

void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
int vsnprintf(char *out, size_t size, const char *format, va_list args);
int snprintf(char *out, size_t size, const char *format, ...);
int fprintf(FILE *f, const char *format, ...);
int fputs(const char *s, FILE *f);
int fflush(FILE *f);
ssize_t write(int fd, const void *data, size_t n);
_Noreturn void exit(int status);
_Noreturn void _Exit(int status);
_Noreturn void abort(void);
int atexit(void (*fn)(void));
char *getenv(const char *name);

typedef long jmp_buf[8];
int _setjmp(jmp_buf env);
_Noreturn void _longjmp(jmp_buf env, int value);
#define setjmp _setjmp
#define longjmp _longjmp
#endif

#ifdef OCEAN_WASM_IMPL
FILE ocean_wasm_stdout = {1}, ocean_wasm_stderr = {2};
FILE *stdout = &ocean_wasm_stdout, *stderr = &ocean_wasm_stderr;

/* ---- memory ---- */
void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b; }
    return 0;
}
char *strchr(const char *s, int c) {
    for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; }
}

/* A first-fit free list over linear memory; each block carries a 16-byte
 * header holding its usable size (keeps 16-byte alignment). */
typedef struct OcBlock { size_t size; struct OcBlock *next; } OcBlock;
#define OC_HEADER 16
extern unsigned char __heap_base;
static unsigned char *heap_next, *heap_end;
static OcBlock *free_blocks;

static void *carve(size_t n) {
    if (!heap_next) { heap_next = &__heap_base; heap_end = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536); }
    heap_next = (unsigned char *)(((uintptr_t)heap_next + 15) & ~(uintptr_t)15);
    if (n > SIZE_MAX - OC_HEADER) return NULL;
    size_t need = OC_HEADER + n;
    if ((size_t)(heap_end - heap_next) < need) {
        size_t shortfall = need - (size_t)(heap_end - heap_next);
        size_t pages = shortfall / 65536 + (shortfall % 65536 != 0);
        if (__builtin_wasm_memory_grow(0, pages) == (size_t)-1) return NULL;
        heap_end += pages * 65536;
    }
    OcBlock *b = (OcBlock *)heap_next;
    b->size = n;
    heap_next += need;
    return (unsigned char *)b + OC_HEADER;
}
void *malloc(size_t n) {
    if (n > SIZE_MAX - 15 - OC_HEADER) return NULL;
    n = (n + 15) & ~(size_t)15;
    if (!n) n = 16;
    for (OcBlock **link = &free_blocks; *link; link = &(*link)->next) {
        OcBlock *b = *link;
        if (b->size >= n && (n > (SIZE_MAX - 64) / 2 || b->size <= 2 * n + 64)) { *link = b->next; return (unsigned char *)b + OC_HEADER; }
    }
    return carve(n);
}
void free(void *p) {
    if (!p) return;
    OcBlock *b = (OcBlock *)((unsigned char *)p - OC_HEADER);
    b->next = free_blocks;
    free_blocks = b;
}
void *calloc(size_t count, size_t size) {
    size_t n = count * size;
    if (size && n / size != count) return NULL;
    void *p = malloc(n);
    if (p) memset(p, 0, n);
    return p;
}
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    OcBlock *b = (OcBlock *)((unsigned char *)p - OC_HEADER);
    if (b->size >= n) return p;
    void *q = malloc(n);
    if (q) { memcpy(q, p, b->size); free(p); }
    return q;
}

/* ---- output ---- */
__attribute__((import_module("env"), import_name("ocean_write")))
extern int ocean_host_write(int fd, const void *data, int n);
ssize_t write(int fd, const void *data, size_t n) { return ocean_host_write(fd, data, (int)n); }

static void put_char(char *out, size_t size, size_t *len, char c) {
    if (*len + 1 < size) out[*len] = c;
    (*len)++;
}
int vsnprintf(char *out, size_t size, const char *f, va_list args) {
    size_t len = 0;
    for (; *f; f++) {
        if (*f != '%') { put_char(out, size, &len, *f); continue; }
        f++;
        int longs = 0;
        while (*f == 'l' || *f == 'z') { longs++; f++; }
        if (*f == '%') { put_char(out, size, &len, '%'); continue; }
        if (*f == 's') { const char *s = va_arg(args, const char *); if (!s) s = "(null)"; while (*s) put_char(out, size, &len, *s++); continue; }
        if (*f == 'c') { put_char(out, size, &len, (char)va_arg(args, int)); continue; }
        if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x' || *f == 'p') {
            unsigned long long v; int neg = 0;
            if (*f == 'p') v = (uintptr_t)va_arg(args, void *);
            else if (*f == 'd' || *f == 'i') {
                long long s = longs >= 2 ? va_arg(args, long long) : longs == 1 ? va_arg(args, long) : va_arg(args, int);
                if (s < 0) { neg = 1; v = 0ull - (unsigned long long)s; } else v = (unsigned long long)s;
            } else v = longs >= 2 ? va_arg(args, unsigned long long) : longs == 1 ? va_arg(args, unsigned long) : va_arg(args, unsigned);
            unsigned base = (*f == 'x' || *f == 'p') ? 16 : 10;
            char digits[24]; int k = 0;
            do { digits[k++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
            if (neg) put_char(out, size, &len, '-');
            while (k) put_char(out, size, &len, digits[--k]);
            continue;
        }
        if (!*f) break;
    }
    if (size) out[len < size ? len : size - 1] = 0;
    return (int)len;
}
int snprintf(char *out, size_t size, const char *format, ...) {
    va_list args; va_start(args, format);
    int n = vsnprintf(out, size, format, args);
    va_end(args);
    return n;
}
int fputs(const char *s, FILE *f) { write(f->fd, s, strlen(s)); return 0; }
int fprintf(FILE *f, const char *format, ...) {
    char buffer[512];
    va_list args; va_start(args, format);
    int n = vsnprintf(buffer, sizeof buffer, format, args);
    va_end(args);
    write(f->fd, buffer, strlen(buffer));
    return n;
}
int fflush(FILE *f) { (void)f; return 0; }

/* ---- process ---- */
_Noreturn void abort(void) { __builtin_trap(); }
_Noreturn void exit(int status) { (void)status; __builtin_trap(); }
_Noreturn void _Exit(int status) { (void)status; __builtin_trap(); }
int atexit(void (*fn)(void)) { (void)fn; return 0; }
char *getenv(const char *name) { (void)name; return NULL; }
int _setjmp(jmp_buf env) { (void)env; return 0; }
_Noreturn void _longjmp(jmp_buf env, int value) { (void)env; (void)value; __builtin_trap(); }
#endif
