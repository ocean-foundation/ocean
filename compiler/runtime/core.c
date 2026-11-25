/* Ocean core runtime: memory, collections, boxing, equality, panics, startup.
 * Embedded in the compiler and compiled with each program by Clang.
 * The type descriptor layout must match the compiler's backend exactly. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

/* libm-free helpers: emitted modules link without -lm. */
static int integral(double f) {
    if (f != f) return 0;
    if (f >= 9223372036854775808.0 || f <= -9223372036854775808.0) return f - f == 0;
    return (double)(int64_t)f == f;
}
typedef struct OcType OcType;
typedef struct { const char *name; const OcType *type; int64_t offset, bitOffset, bits, valueParam, unitSize; } OcField;
typedef struct { const char *name; int64_t nfields; const OcField *fields; } OcVariant;
typedef struct { const char *name; void *fn; } OcMethod;
struct OcType {
    int64_t kind, size, align, bits, flags;
    const OcType *elem, *key;
    const char *name;
    int64_t nfields; const OcField *fields;
    int64_t nvariants; const OcVariant *variants;
    int64_t payloadOffset;
    int64_t nmethods; const OcMethod *methods;
    const OcType *under;
};
enum { K_INT = 1, K_FLOAT, K_BOOL, K_STRING, K_PTR, K_DYN, K_ARRAY, K_BUFFER, K_MAP, K_STRUCT, K_ENUM, K_IFACE, K_FUNC, K_BUFLIST };
enum { F_SIGNED = 1, F_OWNED = 2, F_VARW = 4, F_MIXED = 8, F_NAMED = 16 };

typedef struct OcArr { int64_t len, cap; char *data; } OcArr;
typedef struct { const OcType *t; void *data; } OcAny;
typedef struct { const char *file; int64_t line, col; const char *text; } OcSite;
typedef struct OcDefer {
    struct OcDefer *next;
    void (*thunk)(struct OcDefer *, int32_t invoke);
} OcDefer;
typedef struct OcLocal { void *slot; const struct OcType *t; } OcLocal;
/* locals: the frame's owned locals (null-slot terminated), released when a
 * panic unwinds past the frame. */
typedef struct OcFrame { struct OcFrame *prev; const char *name; int64_t site; OcDefer *defers; void *jb; const OcLocal *locals; int64_t in_defer; OcDefer *active_defers; } OcFrame;
typedef struct OcBufNode { struct OcBufNode *next; void *blk; const OcType *elem; } OcBufNode;

extern const OcSite oc_sites[];
extern const int64_t oc_nsites;
__thread OcFrame *oc_top;
__thread int64_t oc_in_defer;

#define FORWARD_BIT ((int64_t)1 << 40)

static void oc_trace(const char *header, size_t length, int64_t site, OcFrame *from);
void oc_panic(OcArr *msg, int64_t site);

/* ---------------- memory ---------------- */
static size_t checked_add(size_t a, size_t b) {
    size_t r;
    if (__builtin_add_overflow(a, b, &r)) __builtin_trap();
    return r;
}
static size_t checked_mul(size_t a, size_t b) {
    if (b && a > SIZE_MAX / b) __builtin_trap();
    return a * b;
}
static size_t checked_i64_size(int64_t n) {
    if (n < 0 || (uint64_t)n > SIZE_MAX) __builtin_trap();
    return (size_t)n;
}
static int64_t checked_i64_add(int64_t a, int64_t b) {
    int64_t r;
    if (__builtin_add_overflow(a, b, &r)) __builtin_trap();
    return r;
}
static int64_t checked_i64_mul(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    if (a > 0) {
        if (b > 0) { if (a > INT64_MAX / b) __builtin_trap(); }
        else { if (b < INT64_MIN / a) __builtin_trap(); }
    } else {
        if (b > 0) { if (a < INT64_MIN / b) __builtin_trap(); }
        else { if (a == INT64_MIN || b < INT64_MAX / a) __builtin_trap(); }
    }
    return (int64_t)((uint64_t)a * (uint64_t)b);
}
static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) __builtin_trap();
    return p;
}
static void *xcalloc(size_t count, size_t sz) {
    size_t tot = checked_mul(count, sz);
    void *p = calloc(1, tot ? tot : 1);
    if (!p) __builtin_trap();
    return p;
}
static int64_t alloc_total, alloc_count;
static int alloc_stats = -1;
static void report_allocs(void) {
    fprintf(stderr, "oc_alloc: %lld calls, %lld bytes\n",
        (long long)__atomic_load_n(&alloc_count, __ATOMIC_RELAXED),
        (long long)__atomic_load_n(&alloc_total, __ATOMIC_RELAXED));
}
void *oc_alloc(int64_t n) {
    if (n < 0 || (uint64_t)n > SIZE_MAX) __builtin_trap();
    size_t sz = n == 0 ? 1 : (size_t)n;
    if (__atomic_load_n(&alloc_stats, __ATOMIC_RELAXED) < 0) {
        int enabled = getenv("OC_ALLOC_STATS") != NULL;
        int expected = -1;
        if (__atomic_compare_exchange_n(&alloc_stats, &expected, enabled, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED) && enabled) {
            atexit(report_allocs);
        }
    }
    if (__atomic_load_n(&alloc_stats, __ATOMIC_RELAXED)) {
        __atomic_fetch_add(&alloc_count, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&alloc_total, n, __ATOMIC_RELAXED);
    }
    void *p = calloc(1, sz);
    if (!p) __builtin_trap();
    return p;
}
static void *oc_alloc_size(size_t n) {
    if (n > (size_t)INT64_MAX) __builtin_trap();
    return oc_alloc((int64_t)n);
}
void *oc_realloc(void *p, int64_t n) {
    if (n < 0 || (uint64_t)n > SIZE_MAX) __builtin_trap();
    size_t sz = n == 0 ? 1 : (size_t)n;
    void *q = realloc(p, sz);
    if (!q) __builtin_trap();
    return q;
}
void oc_free(void *p) { if (p) free(p); }
void oc_trap(void) { __builtin_trap(); }

/* ---------------- strings ---------------- */
/* An array is one allocation: a 32-byte header followed by the elements and
 * one zero byte (so string data is always NUL-terminated). */
#define ARR_HDR 32
static OcArr *arr_make(int64_t len, int64_t cap, int64_t es) {
    if (len < 0 || cap < 0 || es < 0) __builtin_trap();
    size_t payload = 0;
    if (cap && es) payload = checked_mul(checked_i64_size(cap), checked_i64_size(es));
    size_t total = checked_add(ARR_HDR, checked_add(payload, 1));
    OcArr *a = oc_alloc_size(total);
    a->len = len; a->cap = cap;
    a->data = (char *)a + ARR_HDR;
    return a;
}
OcArr *oc_str_bytes(const char *p, int64_t n) {
    if (n < 0) __builtin_trap();
    if (n == 0) return NULL;
    if (!p) __builtin_trap();
    OcArr *a = arr_make(n, n, 1);
    memcpy(a->data, p, (size_t)n);
    return a;
}
OcArr *oc_str_c(const char *s) { return s ? oc_str_bytes(s, (int64_t)strlen(s)) : NULL; }
static int64_t alen(const OcArr *a) { return a ? a->len : 0; }
/* String constants are static (capacity -1) and never copied or freed. */
/* Strings stored in variables own their bytes (constants are copied too). */
OcArr *oc_str_copy(OcArr *a) {
    if (!a) return NULL;
    if (!a->len) return arr_make(0, 0, 1);
    return oc_str_bytes(a->data, a->len);
}
OcArr *oc_str_concat(OcArr *a, OcArr *b) {
    int64_t la = alen(a), lb = alen(b);
    int64_t n = checked_i64_add(la, lb);
    if (!n) return NULL;
    if (!lb) return oc_str_copy(a);
    if (!la) return oc_str_copy(b);
    OcArr *r = arr_make(n, n, 1);
    if (la) memcpy(r->data, a->data, (size_t)la);
    if (lb) memcpy(r->data + la, b->data, (size_t)lb);
    return r;
}
OcArr *oc_str_add_byte(OcArr *a, int8_t c, int64_t prepend) {
    int64_t la = alen(a);
    int64_t n = checked_i64_add(la, 1);
    OcArr *r = arr_make(n, n, 1);
    if (prepend) { r->data[0] = c; if (la) memcpy(r->data + 1, a->data, (size_t)la); }
    else { if (la) memcpy(r->data, a->data, (size_t)la); r->data[n - 1] = c; }
    return r;
}
int64_t oc_str_cmp(OcArr *a, OcArr *b) {
    int64_t la = alen(a), lb = alen(b), n = la < lb ? la : lb;
    int c = n ? memcmp(a->data, b->data, (size_t)n) : 0;
    if (c) return c < 0 ? -1 : 1;
    return la < lb ? -1 : la > lb ? 1 : 0;
}
int64_t oc_str_eq(OcArr *a, OcArr *b) {
    int64_t la = alen(a);
    if (la != alen(b)) return 0;
    return !la || !memcmp(a->data, b->data, (size_t)la);
}
/* A temporary NUL-free byte copy for C calls (string.data). */
/* A writable NUL-terminated copy of string data, for C callees. */
void *oc_str_cdup(OcArr *a) {
    int64_t n = alen(a);
    char *p = oc_alloc(checked_i64_add(n, 1));
    if (n) memcpy(p, a->data, (size_t)n);
    return p;
}
void *oc_str_cdata(OcArr *a) { return a && a->len ? a->data : (void *)""; }
void *oc_arr_data(OcArr *a) { return a ? a->data : NULL; }
int64_t oc_arr_len(OcArr *a) { return a ? a->len : 0; }
int64_t oc_arr_cap(OcArr *a) { return a ? (a->cap < 0 ? a->len : a->cap) : 0; }

/* ---------------- bounds and fatal errors ---------------- */
static void put_bytes(const char *s, size_t n) { while (n) { ssize_t w = write(2, s, n); if (w <= 0) return; s += w; n -= (size_t)w; } }
static void put(const char *s) { put_bytes(s, strlen(s)); }
static void die(void);
static void putn(int64_t v) { char b[32]; snprintf(b, sizeof b, "%lld", (long long)v); put(b); }
static const OcSite *site_of(int64_t id) {
    id &= ~FORWARD_BIT;
    if (id <= 0 || id >= oc_nsites) return NULL;
    return &oc_sites[id];
}
static void put_site(int64_t id) {
    const OcSite *s = site_of(id);
    if (!s) { put("<unknown>"); return; }
    put(s->file); put(":"); putn(s->line); put(":"); putn(s->col);
}
static void put_snippet(int64_t id) {
    const OcSite *s = site_of(id);
    if (!s || !s->text) return;
    char prefix[48]; snprintf(prefix, sizeof prefix, " %lld | ", (long long)s->line);
    put(prefix); put(s->text); put("\n");
    for (char *p = prefix; *p; p++) put(*p == '|' ? "|" : " ");
    int64_t caret = s->col - 1; int64_t len = (int64_t)strlen(s->text);
    if (caret > len) caret = len;
    for (int64_t i = 0; i < caret; i++) put(s->text[i] == '\t' ? "\t" : " ");
    put("^\n");
}
/* Print a diagnostic whose innermost location is 'site', then the call trace
 * from frame 'from' outward. The innermost frame reports 'site'. */
static void oc_trace(const char *header, size_t length, int64_t site, OcFrame *from) {
    if (length != SIZE_MAX) put_bytes(header, length); else put(header);
    put("\n  --> "); put_site(site); put("\n");
    put_snippet(site);
    put("Call trace (most recent call first):\n");
    int64_t loc = site;
    for (OcFrame *f = from; f; f = f->prev) {
        if (!f->name) continue;
        put("  at "); put(f->name); put(" ("); put_site(loc); put(")\n");
        if (f->prev) loc = f->prev->site;
    }
}
static void fatal(const char *header, int64_t site) {
    oc_trace(header, SIZE_MAX, site, oc_top);
    die();
}
void oc_bounds(int64_t index, int64_t length, int64_t site) {
    char b[160];
    snprintf(b, sizeof b, "Array index out of bounds: index %lld, length %lld", (long long)index, (long long)length);
    fatal(b, site);
}
void oc_slice_bounds(int64_t lo, int64_t hi, int64_t length, int64_t site) {
    char b[200];
    snprintf(b, sizeof b, "Array slice out of bounds: [%lld:%lld] with length %lld", (long long)lo, (long long)hi, (long long)length);
    fatal(b, site);
}
void oc_data_literal_bounds(int64_t lit, int64_t length, int64_t site) {
    char b[200];
    snprintf(b, sizeof b, "Array data literal too large: literal length %lld exceeds array length %lld", (long long)lit, (long long)length);
    fatal(b, site);
}
void oc_array_replace_bounds(int64_t site) { fatal("Array replacement exceeds the destination's storage", site); }

/* ---------------- deep copy / equality / hash ---------------- */
void oc_copy(void *dst, const void *src, const OcType *t);
void *oc_block_copy(const void *src, const OcType *elem);
void *oc_box(const OcType *t, const void *value);
void oc_arr_free(OcArr **slot, const OcType *elem);
int64_t oc_equal(const void *a, const void *b, const OcType *t);
uint64_t oc_hash(const void *p, const OcType *t);
int64_t oc_any_equal(const OcType *ta, const void *da, const OcType *tb, const void *db);
typedef struct OcMap OcMap;
OcMap *oc_map_copy(OcMap *m);
int64_t oc_map_equal(OcMap *a, OcMap *b);
void oc_buf_drop(void **head);

static int owned(const OcType *t) { return t && (t->flags & F_OWNED); }

OcArr *oc_arr_copy(OcArr *a, const OcType *elem) {
    if (!a || !a->len) return NULL;
    int64_t es = elem->size;
    OcArr *r = arr_make(a->len, a->len, es);
    size_t bytes = checked_mul(checked_i64_size(a->len), checked_i64_size(es));
    if (bytes && a->data) memcpy(r->data, a->data, bytes);
    if (owned(elem)) for (int64_t i = 0; i < a->len; i++) oc_copy(r->data + i * es, a->data + i * es, elem);
    return r;
}
static int64_t array_elems_offset(const OcType *elem) { int64_t a = elem->align; return a > 8 ? a : 8; }
static int64_t block_count(const void *block, const OcType *elem) {
    int64_t bytes = *(const int64_t *)block;
    return elem->size ? bytes / elem->size : 0;
}
static void read_bits(const void *base, const OcField *f, void *out, const OcType *t);
OcArr *oc_arr_new(int64_t len, int64_t es, int64_t site);

/* Deep-copy owned contents of the value at dst (already shallow-copied from src). */
void oc_copy(void *dst, const void *src, const OcType *t) {
    if (dst != src) memmove(dst, src, (size_t)t->size);
    if (!owned(t)) return;
    switch (t->kind) {
    case K_STRING: *(OcArr **)dst = oc_str_copy(*(OcArr **)dst); break;
    case K_IFACE: {
        OcAny *x = dst;
        if (x->t) x->data = oc_box(x->t, x->t->kind == K_BUFFER && x->t->bits <= 0 ? (void *)&x->data :
                                   (x->t->kind == K_ARRAY && x->t->bits < 0) ? (void *)&x->data : x->data);
        break;
    }
    case K_DYN: *(OcArr **)dst = oc_arr_copy(*(OcArr *const *)src, t->elem); break;
    case K_MAP: *(OcMap **)dst = oc_map_copy(*(OcMap *const *)src); break;
    case K_ARRAY: {
        if (t->bits < 0) { /* runtime-sized/erased arrays are owned block pointers */
            void *b = *(void **)dst;
            *(void **)dst = b ? oc_block_copy(b, t->elem) : NULL;
            break;
        }
        int64_t off = array_elems_offset(t->elem), n = block_count(src, t->elem);
        for (int64_t i = 0; i < n; i++) oc_copy((char *)dst + off + i * t->elem->size, (char *)dst + off + i * t->elem->size, t->elem);
        break;
    }
    case K_BUFFER:
        if (t->bits > 0) for (int64_t i = 0; i < t->bits; i++) oc_copy((char *)dst + i * t->elem->size, (char *)dst + i * t->elem->size, t->elem);
        break;
    case K_STRUCT:
        for (int64_t i = 0; i < t->nfields; i++) {
            const OcField *f = &t->fields[i];
            if (!f->bits && owned(f->type)) oc_copy((char *)dst + f->offset, (char *)dst + f->offset, f->type);
        }
        break;
    case K_ENUM: {
        int64_t tag = *(int64_t *)dst;
        if (tag < 0 || tag >= t->nvariants) break;
        const OcVariant *v = &t->variants[tag];
        for (int64_t i = 0; i < v->nfields; i++) {
            const OcField *f = &v->fields[i];
            char *p = (char *)dst + t->payloadOffset + f->offset;
            if (owned(f->type)) oc_copy(p, p, f->type);
        }
        break;
    }
    default: break;
    }
}

/* Zero a value in place and set the size headers of inline fixed arrays. */
static void fix_headers(char *p, const OcType *t) {
    switch (t->kind) {
    case K_ARRAY:
        if (t->bits < 0) return;
        *(int64_t *)p = t->bits * t->elem->size;
        if (t->elem->kind == K_ARRAY || t->elem->kind == K_STRUCT || t->elem->kind == K_BUFFER || t->elem->kind == K_ENUM)
            for (int64_t i = 0; i < t->bits; i++) fix_headers(p + array_elems_offset(t->elem) + i * t->elem->size, t->elem);
        break;
    case K_BUFFER:
        if (t->bits > 0 && (t->elem->kind == K_ARRAY || t->elem->kind == K_STRUCT || t->elem->kind == K_BUFFER || t->elem->kind == K_ENUM))
            for (int64_t i = 0; i < t->bits; i++) fix_headers(p + i * t->elem->size, t->elem);
        break;
    case K_STRUCT:
        for (int64_t i = 0; i < t->nfields; i++) if (!t->fields[i].bits) fix_headers(p + t->fields[i].offset, t->fields[i].type);
        break;
    case K_ENUM:
        if (t->nvariants > 0) for (int64_t i = 0; i < t->variants[0].nfields; i++) fix_headers(p + t->payloadOffset + t->variants[0].fields[i].offset, t->variants[0].fields[i].type);
        break;
    default: break;
    }
}
void oc_zero(void *p, const OcType *t) { memset(p, 0, (size_t)t->size); fix_headers(p, t); }
void oc_init_block(char *blk, const OcType *elem) {
    int64_t n = block_count(blk, elem), off = array_elems_offset(elem);
    for (int64_t i = 0; i < n; i++) fix_headers(blk + off + i * elem->size, elem);
}
OcArr *oc_arr_new_init(int64_t len, const OcType *elem, int64_t site) {
    OcArr *a = oc_arr_new(len, elem->size, site);
    for (int64_t i = 0; i < len; i++) fix_headers(a->data + i * elem->size, elem);
    return a;
}
void oc_set_tag(void *p, int64_t tag, const OcType *t) {
    memset(p, 0, (size_t)t->size);
    *(int64_t *)p = tag;
    if (tag >= 0 && tag < t->nvariants)
        for (int64_t i = 0; i < t->variants[tag].nfields; i++) fix_headers((char *)p + t->payloadOffset + t->variants[tag].fields[i].offset, t->variants[tag].fields[i].type);
}

/* Copy a fixed-array block of runtime length into fresh heap storage. */
void *oc_block_copy(const void *src, const OcType *elem) {
    int64_t off = array_elems_offset(elem);
    int64_t bytes = src ? *(const int64_t *)src : 0;
    if (bytes < 0) __builtin_trap();
    char *r = oc_alloc(checked_i64_add(off, bytes));
    if (src) memcpy(r, src, checked_i64_size(checked_i64_add(off, bytes)));
    else *(int64_t *)r = 0;
    if (owned(elem)) {
        int64_t n = elem->size ? bytes / elem->size : 0;
        for (int64_t i = 0; i < n; i++) oc_copy(r + off + i * elem->size, r + off + i * elem->size, elem);
    }
    return r;
}
void oc_block_copy_elems(void *blk, const OcType *elem) {
    if (!blk || !elem || !owned(elem)) return;
    int64_t off = array_elems_offset(elem);
    int64_t n = block_count(blk, elem);
    for (int64_t i = 0; i < n; i++) {
        char *p = (char *)blk + off + i * elem->size;
        oc_copy(p, p, elem);
    }
}
void *oc_block_new(int64_t count, const OcType *elem) {
    if (count < 0) __builtin_trap();
    int64_t off = array_elems_offset(elem);
    int64_t payload = checked_i64_mul(count, elem->size);
    char *r = oc_alloc(checked_i64_add(off, payload));
    *(int64_t *)r = payload;
    return r;
}

static int64_t sext_bits(uint64_t v, int64_t bits) {
    if (bits >= 64) return (int64_t)v;
    uint64_t m = (uint64_t)1 << (bits - 1);
    v &= ((uint64_t)1 << bits) - 1;
    return (int64_t)((v ^ m) - m);
}
static uint64_t load_u(const void *p, int64_t size) {
    uint64_t v = 0;
    memcpy(&v, p, (size_t)(size > 8 ? 8 : size));
    return v;
}

/* Numeric view of a boxed number for exact cross-type comparison. */
typedef struct { int kind; /* 0 int, 1 float */ int neg; uint64_t mag[4]; int wide; double f; long double ld; } Num;
static int num_of(const OcType *t, const void *p, int64_t *ival, uint64_t *uval, double *fval, int *isunsigned) {
    if (t->kind == K_FLOAT) { *fval = t->bits == 32 ? *(const float *)p : *(const double *)p; return 2; }
    if (t->kind == K_BOOL) return 0;
    if (t->kind != K_INT) return 0;
    if (t->bits > 64) return 3;
    uint64_t raw = load_u(p, t->size);
    if (t->bits < 64) raw &= ((uint64_t)1 << t->bits) - 1;
    if (t->flags & F_SIGNED) { *ival = sext_bits(raw, t->bits); *isunsigned = 0; }
    else { *uval = raw; *isunsigned = 1; }
    return 1;
}
static int wide_is_neg(const OcType *t, const void *p) {
    if (!(t->flags & F_SIGNED) || t->bits <= 0) return 0;
    int64_t n = (t->bits + 7) / 8;
    int top = (int)((t->bits - 1) % 8);
    return (((const unsigned char *)p)[n - 1] >> top) & 1;
}
static int wide_byte(const OcType *t, const void *p, int64_t idx, int neg) {
    int64_t n = (t->bits + 7) / 8;
    if (idx >= n) return neg ? 0xff : 0;
    unsigned char c = ((const unsigned char *)p)[idx];
    int64_t bits_in_byte = t->bits - idx * 8;
    if (bits_in_byte < 8) {
        unsigned int mask = (1u << bits_in_byte) - 1u;
        c = (unsigned char)((c & mask) | (neg ? (~mask & 0xffu) : 0u));
    }
    return c;
}
static int wide_equal(const OcType *ta, const void *a, const OcType *tb, const void *b) {
    int neg_a = wide_is_neg(ta, a), neg_b = wide_is_neg(tb, b);
    if (neg_a != neg_b) return 0;
    int64_t na = (ta->bits + 7) / 8, nb = (tb->bits + 7) / 8, n = na > nb ? na : nb;
    for (int64_t i = 0; i < n; i++) {
        if (wide_byte(ta, a, i, neg_a) != wide_byte(tb, b, i, neg_b)) return 0;
    }
    return 1;
}
/* Exact comparison of a wide integer with a double. */
static int wide_float_equal(const OcType *t, const void *v, double f) {
    if (f != f || f - f != 0) return 0;          /* NaN or infinity */
    if (!integral(f)) return 0;
    int64_t n = (t->bits + 7) / 8;
    unsigned char stack_mag[256];
    size_t mag_sz = checked_add(checked_i64_size(n), 1);
    unsigned char *mag = mag_sz <= sizeof(stack_mag) ? stack_mag : xmalloc(mag_sz);
    const unsigned char *bytes = v;
    int neg = 0;
    memcpy(mag, bytes, (size_t)n); mag[n] = 0;
    int top = (int)((t->bits - 1) % 8);
    if (t->flags & F_SIGNED) {
        neg = (bytes[n - 1] >> top) & 1;
        if (neg) {                               /* two's complement magnitude */
            for (int top_bits = top + 1; top_bits < 8; top_bits++) mag[n - 1] |= (unsigned char)(1 << top_bits);
            int carry = 1;
            for (int64_t i = 0; i < n; i++) { int x = (unsigned char)~mag[i] + carry; mag[i] = (unsigned char)x; carry = x >> 8; }
        } else if (top < 7) mag[n - 1] &= (unsigned char)((1 << (top + 1)) - 1);
    } else if (top < 7) mag[n - 1] &= (unsigned char)((1 << (top + 1)) - 1);
    if ((f < 0) != neg && f != 0) { if (mag != stack_mag) free(mag); return 0; }
    double af = f < 0 ? -f : f;
    uint64_t bitsf; memcpy(&bitsf, &af, 8);      /* af = m * 2^shift, from the IEEE fields */
    int exp = (int)((bitsf >> 52) & 0x7ff);
    uint64_t m = bitsf & ((1ull << 52) - 1); int shift;
    if (exp == 0) shift = -1074; else { m |= 1ull << 52; shift = exp - 1075; }
    if (af == 0) { m = 0; shift = 0; }
    while (shift < 0 && m && !(m & 1)) { m >>= 1; shift++; }
    if (shift < 0) { if (mag != stack_mag) free(mag); return 0; }
    /* compare mag with m << shift byte by byte */
    int64_t total = n + 1 > (shift + 64) / 8 + 2 ? n + 1 : (shift + 64) / 8 + 2;
    int eq = 1;
    for (int64_t i = 0; i < total; i++) {
        int64_t bit = i * 8 - shift;            /* bits of m landing in byte i */
        unsigned int want = 0;
        for (int k = 0; k < 8; k++) { int64_t mb = bit + k; if (mb >= 0 && mb < 64 && ((m >> mb) & 1)) want |= 1u << k; }
        unsigned int have = i <= n ? mag[i] : 0;
        if (have != want) { eq = 0; break; }
    }
    if (mag != stack_mag) free(mag);
    return eq;
}
static int numeric_equal(const OcType *ta, const void *a, const OcType *tb, const void *b) {
    int64_t ia = 0, ib = 0; uint64_t ua = 0, ub = 0; double fa = 0, fb = 0; int sa = 0, sb = 0;
    int ka = num_of(ta, a, &ia, &ua, &fa, &sa), kb = num_of(tb, b, &ib, &ub, &fb, &sb);
    if (ka == 3 || kb == 3) {
        if (ka == 3 && kb == 3) return wide_equal(ta, a, tb, b);
        /* wide vs narrow: compare via 64-bit when the wide value fits */
        const OcType *tw = ka == 3 ? ta : tb; const void *w = ka == 3 ? a : b;
        int64_t n = (tw->bits + 7) / 8;
        int neg = wide_is_neg(tw, w);
        int other = ka == 3 ? kb : ka;
        if (other == 2) { double kf2 = 0; int64_t x1 = 0; uint64_t x2 = 0; int x3 = 0; num_of(ka == 3 ? tb : ta, ka == 3 ? b : a, &x1, &x2, &kf2, &x3); return wide_float_equal(tw, w, kf2); }
        for (int64_t i = 8; i < n; i++) if (wide_byte(tw, w, i, neg) != (neg ? 0xff : 0)) return 0;
        uint64_t low = load_u(w, 8);
        int64_t ki = 0; uint64_t ku = 0; double kf = 0; int ks = 0;
        if (other == 1) {
            num_of(ka == 3 ? tb : ta, ka == 3 ? b : a, &ki, &ku, &kf, &ks);
            if (ks) return !neg && low == ku;
            return neg ? ((int64_t)low < 0 && (int64_t)low == ki) : (low <= (uint64_t)INT64_MAX && (int64_t)low == ki);
        }
        return 0;
    }
    if (ka == 2 || kb == 2) {
        if (ka == 2 && kb == 2) return fa == fb;
        double f = ka == 2 ? fa : fb;
        int us = ka == 2 ? sb : sa; int64_t iv = ka == 2 ? ib : ia; uint64_t uv = ka == 2 ? ub : ua;
        if (!integral(f)) return 0;
        if (us) return f >= 0 && f < 18446744073709551616.0 && (uint64_t)f == uv;
        return f >= -9223372036854775808.0 && f < 9223372036854775808.0 && (int64_t)f == iv;
    }
    if (ka == 1 && kb == 1) {
        if (sa && sb) return ua == ub;
        if (!sa && !sb) return ia == ib;
        if (sa) return ib >= 0 && (uint64_t)ib == ua;
        return ia >= 0 && (uint64_t)ia == ub;
    }
    return 0;
}
static int is_number(const OcType *t) { return t && (t->kind == K_INT || t->kind == K_FLOAT); }

static uint64_t mix(uint64_t h) { h ^= h >> 33; h *= 0xff51afd7ed558ccdULL; h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ULL; h ^= h >> 33; return h; }
static uint64_t hash_bytes(const void *p, int64_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (int64_t i = 0; i < n; i++) { h ^= ((const unsigned char *)p)[i]; h *= 1099511628211ULL; }
    return h;
}
static uint64_t hash_number(const OcType *t, const void *p) {
    int64_t iv = 0; uint64_t uv = 0; double fv = 0; int us = 0;
    int k = num_of(t, p, &iv, &uv, &fv, &us);
    if (k == 2) {
        if (fv == 0) return mix(0);
        if (integral(fv) && fv >= -9223372036854775808.0 && fv < 9223372036854775808.0) return mix((uint64_t)(int64_t)fv);
        if (integral(fv) && fv >= 0 && fv < 18446744073709551616.0) return mix((uint64_t)fv);
        uint64_t bits; memcpy(&bits, &fv, 8); return mix(bits ^ 0x9e3779b97f4a7c15ULL);
    }
    if (k == 1) return mix(us ? uv : (uint64_t)iv);
    if (k == 3) {
        int64_t n = (t->bits + 7) / 8;
        int neg = wide_is_neg(t, p);
        int fits = 1;
        for (int64_t i = 8; i < n; i++) if (wide_byte(t, p, i, neg) != (neg ? 0xff : 0)) { fits = 0; break; }
        uint64_t low = load_u(p, 8);
        if (fits && (!neg || (int64_t)low < 0)) return mix(low);
        int64_t used = n;
        while (used > 1) {
            int top = wide_byte(t, p, used - 1, neg);
            int prev = wide_byte(t, p, used - 2, neg);
            if (neg && top == 0xff && (prev & 0x80)) used--;
            else if (!neg && top == 0x00) used--;
            else break;
        }
        uint64_t h = 1469598103934665603ULL ^ (uint64_t)(neg ? 1 : 0);
        for (int64_t i = 0; i < used; i++) { h ^= (unsigned char)wide_byte(t, p, i, neg); h *= 1099511628211ULL; }
        return mix(h);
    }
    return 0;
}

uint64_t oc_any_hash(const OcType *t, const void *d) {
    if (!t) return 7;
    if (is_number(t)) return hash_number(t, d);
    if (t->kind == K_BUFFER && t->bits <= 0) return mix((uint64_t)(uintptr_t)d);
    if (t->kind == K_ARRAY && t->bits < 0) return mix(oc_hash(&d, t) + (uint64_t)(uintptr_t)t);
    return mix(oc_hash(d, t) + (uint64_t)(uintptr_t)t);
}

uint64_t oc_hash(const void *p, const OcType *t) {
    switch (t->kind) {
    case K_INT: case K_FLOAT: return hash_number(t, p);
    case K_BOOL: return mix(*(const unsigned char *)p & 1);
    case K_STRING: case K_DYN: {
        const OcArr *a = *(OcArr *const *)p;
        if (!a || !a->len) return 3;
        if (t->kind == K_STRING || (t->elem->kind == K_INT && t->elem->size == 1)) return hash_bytes(a->data, a->len);
        uint64_t h = 5;
        for (int64_t i = 0; i < a->len; i++) h = mix(h * 31 + oc_hash(a->data + i * t->elem->size, t->elem));
        return h;
    }
    case K_PTR: return mix(load_u(p, t->size));
    case K_FUNC: return hash_bytes(p, t->size);
    case K_ARRAY: {
        if (t->bits < 0) { p = *(void *const *)p; if (!p) return 11; }
        int64_t off = array_elems_offset(t->elem), n = block_count(p, t->elem); uint64_t h = 11;
        for (int64_t i = 0; i < n; i++) h = mix(h * 31 + oc_hash((const char *)p + off + i * t->elem->size, t->elem));
        return h;
    }
    case K_BUFFER: {
        if (t->bits <= 0) return mix(load_u(p, t->size));
        uint64_t h = 13;
        for (int64_t i = 0; i < t->bits; i++) h = mix(h * 31 + oc_hash((const char *)p + i * t->elem->size, t->elem));
        return h;
    }
    case K_STRUCT: {
        uint64_t h = 17;
        for (int64_t i = 0; i < t->nfields; i++) {
            const OcField *f = &t->fields[i];
            if (f->bits) {
                char stack_tmp[16] = {0};
                char *tmp = f->type->size > 16 ? xcalloc(1, checked_i64_size(f->type->size)) : stack_tmp;
                read_bits(p, f, tmp, f->type);
                uint64_t fh = oc_hash(tmp, f->type);
                if (tmp != stack_tmp) free(tmp);
                h = mix(h * 31 + fh);
            } else h = mix(h * 31 + oc_hash((const char *)p + f->offset, f->type));
        }
        return h;
    }
    case K_ENUM: {
        int64_t tag = *(const int64_t *)p; uint64_t h = mix((uint64_t)tag + 19);
        if (tag >= 0 && tag < t->nvariants) {
            const OcVariant *v = &t->variants[tag];
            for (int64_t i = 0; i < v->nfields; i++) h = mix(h * 31 + oc_hash((const char *)p + t->payloadOffset + v->fields[i].offset, v->fields[i].type));
        }
        return h;
    }
    case K_IFACE: { const OcAny *a = p; return oc_any_hash(a->t, a->data); }
    case K_MAP: return 23;
    }
    return 0;
}

static void read_bits(const void *base, const OcField *f, void *out, const OcType *t) {
    int64_t usz = f->unitSize > 0 ? f->unitSize : 8;
    if (usz > 8) usz = 8;
    uint64_t unit = load_u((const char *)base + f->offset, usz);
    uint64_t mask = f->bits >= 64 ? ~0ULL : (((uint64_t)1 << f->bits) - 1);
    uint64_t v = (unit >> f->bitOffset) & mask;
    if ((t->flags & F_SIGNED) && f->bits < 64 && ((v >> (f->bits - 1)) & 1)) v |= ~mask;
    memcpy(out, &v, (size_t)(t->size > 8 ? 8 : checked_i64_size(t->size)));
    if (t->size > 8) memset((char *)out + 8, (t->flags & F_SIGNED) && ((int64_t)v < 0) ? 0xff : 0, (size_t)(t->size - 8));
}

int64_t oc_equal(const void *a, const void *b, const OcType *t) {
    switch (t->kind) {
    case K_INT: {
        if (t->bits > 64) return wide_equal(t, a, t, b);
        uint64_t x = load_u(a, t->size), y = load_u(b, t->size);
        if (t->bits < 64) { uint64_t m = ((uint64_t)1 << t->bits) - 1; x &= m; y &= m; }
        return x == y;
    }
    case K_FLOAT: return t->bits == 32 ? *(const float *)a == *(const float *)b : *(const double *)a == *(const double *)b;
    case K_BOOL: return (*(const unsigned char *)a & 1) == (*(const unsigned char *)b & 1);
    case K_STRING: return oc_str_eq(*(OcArr *const *)a, *(OcArr *const *)b);
    case K_PTR: return load_u(a, t->size) == load_u(b, t->size);
    case K_FUNC: return t->size ? !memcmp(a, b, (size_t)t->size) : 1;
    case K_DYN: {
        const OcArr *x = *(OcArr *const *)a, *y = *(OcArr *const *)b;
        int64_t n = alen(x);
        if (n != alen(y)) return 0;
        for (int64_t i = 0; i < n; i++) if (!oc_equal(x->data + i * t->elem->size, y->data + i * t->elem->size, t->elem)) return 0;
        return 1;
    }
    case K_ARRAY: {
        if (t->bits < 0) {
            a = *(void *const *)a; b = *(void *const *)b;
            if (!a || !b) return (a ? block_count(a, t->elem) : 0) == (b ? block_count(b, t->elem) : 0);
        }
        int64_t off = array_elems_offset(t->elem), n = block_count(a, t->elem);
        if (n != block_count(b, t->elem)) return 0;
        for (int64_t i = 0; i < n; i++) if (!oc_equal((const char *)a + off + i * t->elem->size, (const char *)b + off + i * t->elem->size, t->elem)) return 0;
        return 1;
    }
    case K_BUFFER:
        if (t->bits <= 0) return load_u(a, t->size) == load_u(b, t->size);
        for (int64_t i = 0; i < t->bits; i++) if (!oc_equal((const char *)a + i * t->elem->size, (const char *)b + i * t->elem->size, t->elem)) return 0;
        return 1;
    case K_STRUCT:
        for (int64_t i = 0; i < t->nfields; i++) {
            const OcField *f = &t->fields[i];
            if (f->bits) {
                char sx[16] = {0}, sy[16] = {0};
                char *x = f->type->size > 16 ? xcalloc(1, checked_i64_size(f->type->size)) : sx;
                char *y = f->type->size > 16 ? xcalloc(1, checked_i64_size(f->type->size)) : sy;
                read_bits(a, f, x, f->type); read_bits(b, f, y, f->type);
                int64_t eq = oc_equal(x, y, f->type);
                if (x != sx) free(x);
                if (y != sy) free(y);
                if (!eq) return 0;
            } else if (!oc_equal((const char *)a + f->offset, (const char *)b + f->offset, f->type)) return 0;
        }
        return 1;
    case K_ENUM: {
        int64_t ta = *(const int64_t *)a, tb = *(const int64_t *)b;
        if (ta != tb) return 0;
        if (ta < 0 || ta >= t->nvariants) return 1;
        const OcVariant *v = &t->variants[ta];
        for (int64_t i = 0; i < v->nfields; i++) {
            const OcField *f = &v->fields[i];
            if (!oc_equal((const char *)a + t->payloadOffset + f->offset, (const char *)b + t->payloadOffset + f->offset, f->type)) return 0;
        }
        return 1;
    }
    case K_IFACE: { const OcAny *x = a, *y = b; return oc_any_equal(x->t, x->data, y->t, y->data); }
    case K_MAP: return oc_map_equal(*(OcMap *const *)a, *(OcMap *const *)b);
    }
    return 0;
}

int64_t oc_elems_equal(const char *a, int64_t na, const char *b, int64_t nb, const OcType *e) {
    if (na != nb) return 0;
    for (int64_t i = 0; i < na; i++) if (!oc_equal(a + i * e->size, b + i * e->size, e)) return 0;
    return 1;
}
int64_t oc_any_equal(const OcType *ta, const void *da, const OcType *tb, const void *db) {
    if (!ta || !tb) return ta == tb;
    if (is_number(ta) && is_number(tb) && !((ta->flags | tb->flags) & F_NAMED)) return numeric_equal(ta, da, tb, db);
    if (ta != tb) return 0;
    if (ta->kind == K_BUFFER && ta->bits <= 0) return da == db;
    if (ta->kind == K_ARRAY && ta->bits < 0) return oc_equal(&da, &db, ta);
    return oc_equal(da, db, ta);
}

/* ---------------- destruction ---------------- */
typedef struct OcMapHdr { int64_t count, cap, tombs; const OcType *kt, *vt; uint64_t *hashes; char *keys, *vals; } OcMapHdr;
static void destroy_boxed(const OcType *t, void *data);
/* Release everything owned by the value stored at slot (slot itself stays). */
void oc_destroy(void *slot, const OcType *t) {
    if (!t || !owned(t)) return;
    switch (t->kind) {
    case K_STRING: {
        OcArr *a = *(OcArr **)slot;
        if (a && a->cap >= 0) free(a);
        break;
    }
    case K_DYN: { OcArr *a = *(OcArr **)slot; oc_arr_free(&a, t->elem); break; }
    case K_MAP: {
        OcMapHdr *m = *(OcMapHdr **)slot;
        if (!m) break;
        for (int64_t i = 0; i < m->cap; i++) {
            if (m->hashes[i] < 2) continue;
            oc_destroy(m->keys + i * m->kt->size, m->kt);
            oc_destroy(m->vals + i * m->vt->size, m->vt);
        }
        free(m->hashes); free(m->keys); free(m->vals); free(m);
        break;
    }
    case K_ARRAY: {
        char *blk = t->bits < 0 ? *(char **)slot : slot;
        if (!blk) break;
        int64_t off = array_elems_offset(t->elem), n = block_count(blk, t->elem);
        if (owned(t->elem)) for (int64_t i = 0; i < n; i++) oc_destroy(blk + off + i * t->elem->size, t->elem);
        if (t->bits < 0) free(blk);
        break;
    }
    case K_BUFFER:
        if (t->bits > 0) for (int64_t i = 0; i < t->bits; i++) oc_destroy((char *)slot + i * t->elem->size, t->elem);
        break;
    case K_STRUCT:
        for (int64_t i = 0; i < t->nfields; i++) {
            const OcField *f = &t->fields[i];
            if (!f->bits) oc_destroy((char *)slot + f->offset, f->type);
        }
        break;
    case K_ENUM: {
        int64_t tag = *(int64_t *)slot;
        if (tag < 0 || tag >= t->nvariants) break;
        const OcVariant *v = &t->variants[tag];
        for (int64_t i = 0; i < v->nfields; i++) oc_destroy((char *)slot + t->payloadOffset + v->fields[i].offset, v->fields[i].type);
        break;
    }
    case K_IFACE: { OcAny *x = slot; if (x->t) destroy_boxed(x->t, x->data); break; }
    case K_BUFLIST: oc_buf_drop((void **)slot); break;
    }
}
static void destroy_boxed(const OcType *t, void *data) {
    if (!data || (t->kind == K_BUFFER && t->bits <= 0)) return;
    if (t->kind == K_ARRAY && t->bits < 0) { oc_destroy(&data, t); return; }
    oc_destroy(data, t);
    free(data);
}
/* Destroy then clear a slot (temporaries, reassignment, scope exit). */
void oc_drop(void *slot, const OcType *t) {
    oc_destroy(slot, t);
    memset(slot, 0, (size_t)t->size);
}
void oc_free_ptr(void *p, const OcType *pointee) {
    if (!p) return;
    if (pointee) oc_destroy(p, pointee);
    free(p);
}
void oc_buf_push(void **head, void *blk, const OcType *elem) {
    if (!head || !blk) return;
    OcBufNode *n = xmalloc(sizeof(OcBufNode));
    n->blk = blk;
    n->elem = elem;
    n->next = (OcBufNode *)*head;
    *head = n;
}
void oc_buf_drop(void **head) {
    if (!head) return;
    OcBufNode *n = (OcBufNode *)*head;
    *head = NULL;
    while (n) {
        OcBufNode *next = n->next;
        if (n->blk) {
            if (n->elem && owned(n->elem)) {
                int64_t off = array_elems_offset(n->elem);
                int64_t cnt = block_count(n->blk, n->elem);
                for (int64_t i = 0; i < cnt; i++) {
                    oc_destroy((char *)n->blk + off + i * n->elem->size, n->elem);
                }
            }
            free(n->blk);
        }
        free(n);
        n = next;
    }
}
const OcType oc_type_buflist = { K_BUFLIST, (int64_t)sizeof(void *), (int64_t)sizeof(void *), 0, F_OWNED, NULL, NULL, "<buflist>", 0, NULL, 0, NULL, 0, 0, NULL, NULL };

void oc_data_destroy_elems(void *dst, int64_t count, const OcType *elem) {
    if (!dst || count <= 0 || !elem || !owned(elem)) return;
    for (int64_t i = 0; i < count; i++) oc_destroy((char *)dst + i * elem->size, elem);
}
void oc_data_copy_elems(void *dst, const void *src, int64_t count, const OcType *elem) {
    if (count <= 0) return;
    if (!dst || !src) __builtin_trap();
    size_t es = checked_i64_size(elem->size);
    size_t bytes = checked_mul(checked_i64_size(count), es);
    if (!owned(elem)) {
        if (bytes) memmove(dst, src, bytes);
        return;
    }
    char *tmp = xmalloc(bytes ? bytes : 1);
    for (int64_t i = 0; i < count; i++) oc_copy(tmp + (size_t)i * es, (const char *)src + (size_t)i * es, elem);
    for (int64_t i = 0; i < count; i++) oc_destroy((char *)dst + (size_t)i * es, elem);
    if (bytes) memcpy(dst, tmp, bytes);
    free(tmp);
}

/* ---------------- boxing ---------------- */
void *oc_box(const OcType *t, const void *value) {
    int64_t size = t->size;
    /* Unsized buffer views box as their address: the view is non-owning. */
    if (t->kind == K_BUFFER && t->bits <= 0) return *(void *const *)value;
    if (t->kind == K_ARRAY && t->bits < 0) return oc_block_copy(*(void *const *)value, t->elem);
    void *d = oc_alloc(size);
    oc_copy(d, value, t);
    return d;
}
/* Unbox into dst with ownership (deep copy). */
void oc_unbox(void *dst, const OcType *t, const void *data) { oc_copy(dst, data, t); }

/* ---------------- dynamic arrays ---------------- */
static void reserve(OcArr **slot, int64_t need, int64_t es) {
    OcArr *a = *slot;
    if (!a) { a = arr_make(0, 0, es); *slot = a; }
    if (need <= a->cap) return;
    int64_t cap = a->cap >= 4 ? (a->cap > INT64_MAX / 2 ? INT64_MAX : a->cap * 2) : 8;
    if (cap < need) cap = need;
    /* malloc/free only (no realloc): allocation tracking tools see every block. */
    OcArr *b = arr_make(a->len, cap, es);
    if (a->len) memcpy(b->data, a->data, checked_mul(checked_i64_size(a->len), checked_i64_size(es)));
    if (a->cap >= 0) free(a);
    *slot = b;
}
/* Open a gap of 'count' elements at 'index'; returns its address. */
void *oc_arr_insert(OcArr **slot, int64_t index, int64_t count, int64_t es, int64_t site) {
    if (count < 0 || es < 0) __builtin_trap();
    int64_t len = alen(*slot);
    if (index < 0 || index > len) oc_bounds(index, len, site);
    int64_t total = checked_i64_add(len, count);
    reserve(slot, total, es);
    OcArr *a = *slot;
    size_t tail_bytes = checked_mul(checked_i64_size(len - index), checked_i64_size(es));
    if (tail_bytes) memmove(a->data + checked_i64_mul(index + count, es), a->data + checked_i64_mul(index, es), tail_bytes);
    a->len = total;
    return a->data + checked_i64_mul(index, es);
}
void oc_arr_insert_from(OcArr **slot, int64_t index, const void *src, int64_t count, const OcType *elem, int64_t site) {
    if (count < 0) __builtin_trap();
    if (count == 0) return;
    int64_t es = elem->size;
    size_t bytes = checked_mul(checked_i64_size(count), checked_i64_size(es));
    if (bytes && !src) __builtin_trap();
    /* the source may alias the destination array: snapshot it first */
    char *copy = bytes ? xmalloc(bytes) : NULL;
    if (bytes) memcpy(copy, src, bytes);
    char *dst = oc_arr_insert(slot, index, count, es, site);
    if (bytes) memcpy(dst, copy, bytes);
    free(copy);
    if (owned(elem)) for (int64_t i = 0; i < count; i++) oc_copy(dst + i * es, dst + i * es, elem);
}
void *oc_arr_push(OcArr **slot, int64_t es) {
    int64_t len = alen(*slot);
    reserve(slot, checked_i64_add(len, 1), es);
    (*slot)->len = len + 1;
    return (*slot)->data + checked_i64_mul(len, es);
}
void oc_arr_remove(OcArr **slot, int64_t index, int64_t es, void *out, int64_t site) {
    OcArr *a = *slot; int64_t len = alen(a);
    if (index < 0 || index >= len) oc_bounds(index, len, site);
    size_t ses = checked_i64_size(es);
    if (ses) memcpy(out, a->data + checked_i64_mul(index, es), ses);
    size_t tail_bytes = checked_mul(checked_i64_size(len - index - 1), ses);
    if (tail_bytes) memmove(a->data + checked_i64_mul(index, es), a->data + checked_i64_mul(index + 1, es), tail_bytes);
    a->len = len - 1;
    if (ses) memset(a->data + checked_i64_mul(a->len, es), 0, ses);
}
void oc_arr_pop(OcArr **slot, int64_t es, void *out, int64_t site) {
    int64_t len = alen(*slot);
    if (len == 0) oc_bounds(-1, 0, site);
    oc_arr_remove(slot, len - 1, es, out, site);
}
OcArr *oc_arr_new(int64_t len, int64_t es, int64_t site) {
    if (len < 0) oc_bounds(len, 0, site);
    if (!len) return NULL;
    return arr_make(len, len, es);
}
OcArr *oc_arr_from(const void *data, int64_t n, int64_t es) {
    if (n < 0) __builtin_trap();
    if (n == 0) return NULL;
    OcArr *a = arr_make(n, n, es);
    size_t bytes = checked_mul(checked_i64_size(n), checked_i64_size(es));
    if (bytes) {
        if (!data) __builtin_trap();
        memcpy(a->data, data, bytes);
    }
    return a;
}
/* New dynamic array of elements [lo, hi) from raw storage (deep copied). */
OcArr *oc_slice(const void *data, int64_t len, int64_t lo, int64_t hi, const OcType *elem, int64_t checked, int64_t site) {
    if (checked && (lo < 0 || hi > len || lo > hi)) oc_slice_bounds(lo, hi, len, site);
    if (!checked && (lo < 0 || lo > hi)) oc_slice_bounds(lo, hi, len, site);
    int64_t n = hi - lo;
    if (n <= 0) return NULL;
    OcArr *a = arr_make(n, n, elem->size);
    size_t bytes = checked_mul(checked_i64_size(n), checked_i64_size(elem->size));
    if (bytes) {
        if (!data) __builtin_trap();
        memcpy(a->data, (const char *)data + checked_i64_mul(lo, elem->size), bytes);
    }
    if (owned(elem)) for (int64_t i = 0; i < n; i++) oc_copy(a->data + i * elem->size, a->data + i * elem->size, elem);
    return a;
}
OcArr *oc_concat(const void *da, int64_t na, const void *db, int64_t nb, const OcType *elem) {
    if (na < 0 || nb < 0) __builtin_trap();
    int64_t n = checked_i64_add(na, nb), es = elem->size;
    if (n == 0) return NULL;
    OcArr *a = arr_make(n, n, es);
    size_t ba = checked_mul(checked_i64_size(na), checked_i64_size(es));
    size_t bb = checked_mul(checked_i64_size(nb), checked_i64_size(es));
    if (ba) memcpy(a->data, da, ba);
    if (bb) memcpy(a->data + ba, db, bb);
    if (owned(elem)) for (int64_t i = 0; i < n; i++) oc_copy(a->data + i * es, a->data + i * es, elem);
    return a;
}
void oc_destroy(void *slot, const OcType *t);
void oc_arr_free(OcArr **slot, const OcType *elem) {
    OcArr *a = *slot;
    if (a && a->cap >= 0) {
        if (owned(elem)) for (int64_t i = 0; i < a->len; i++) oc_destroy(a->data + i * elem->size, elem);
        free(a);
    }
    *slot = NULL;
}
/* Homogeneous interface arrays: elements [start, start+count) must share the
 * dynamic type of the rest of the array (or of element start). */
void oc_hom_range(OcArr *a, int64_t start, int64_t count) {
    int64_t n = alen(a);
    if (n < 2 || count <= 0) return;
    OcAny *e = (OcAny *)a->data;
    int64_t ref = start > 0 ? 0 : (start + count < n ? start + count : start);
    for (int64_t i = start; i < start + count && i < n; i++)
        if (e[i].t != e[ref].t) __builtin_trap();
}

/* Fixed-array block from a dynamic array (array() method). */
void *oc_arr_to_block(OcArr *a, const OcType *elem) {
    int64_t n = alen(a), off = array_elems_offset(elem);
    int64_t payload = checked_i64_mul(n, elem->size);
    char *r = oc_alloc(checked_i64_add(off, payload));
    *(int64_t *)r = payload;
    if (payload) memcpy(r + off, a->data, (size_t)payload);
    if (owned(elem)) for (int64_t i = 0; i < n; i++) oc_copy(r + off + i * elem->size, r + off + i * elem->size, elem);
    return r;
}

/* ---------------- maps ---------------- */
struct OcMap {
    int64_t count, cap, tombs;
    const OcType *kt, *vt;
    uint64_t *hashes;   /* 0 empty, 1 tombstone, else hash|2 */
    char *keys, *vals;
};
static int64_t map_find(const OcMap *m, const void *key, uint64_t h) {
    if (!m || !m->cap) return -1;
    int64_t mask = m->cap - 1, i = (int64_t)(h & (uint64_t)mask);
    for (;;) {
        uint64_t s = m->hashes[i];
        if (s == 0) return -1;
        if (s == h && oc_equal(m->keys + i * m->kt->size, key, m->kt)) return i;
        i = (i + 1) & mask;
    }
}
static uint64_t key_hash(const OcType *kt, const void *key) { return oc_hash(key, kt) | 2; }
static void map_grow(OcMap *m) {
    int64_t old = m->cap; uint64_t *oh = m->hashes; char *ok = m->keys, *ov = m->vals;
    int64_t cap = old ? checked_i64_mul(old, 2) : 8;
    while ((m->count + 1) > cap / 2) cap = checked_i64_mul(cap, 2);
    int64_t ks = m->kt->size ? m->kt->size : 1;
    int64_t vs = m->vt->size ? m->vt->size : 1;
    m->cap = cap; m->tombs = 0;
    m->hashes = oc_alloc(checked_i64_mul(cap, 8));
    m->keys = oc_alloc(checked_i64_mul(cap, ks));
    m->vals = oc_alloc(checked_i64_mul(cap, vs));
    for (int64_t i = 0; i < old; i++) {
        if (oh[i] < 2) continue;
        int64_t j = (int64_t)(oh[i] & (uint64_t)(cap - 1));
        while (m->hashes[j]) j = (j + 1) & (cap - 1);
        m->hashes[j] = oh[i];
        if (m->kt->size) memcpy(m->keys + j * m->kt->size, ok + i * m->kt->size, (size_t)m->kt->size);
        if (m->vt->size) memcpy(m->vals + j * m->vt->size, ov + i * m->vt->size, (size_t)m->vt->size);
    }
    free(oh); free(ok); free(ov);
}
/* Returns the value slot for key, or NULL when absent. */
void *oc_map_get(OcMap *m, const void *key, const OcType *kt) {
    if (!m) return NULL;
    int64_t i = map_find(m, key, key_hash(kt, key));
    return i < 0 ? NULL : m->vals + i * m->vt->size;
}
/* Insert or replace; key and value are copied deeply. */
void oc_map_set(OcMap **slot, const void *key, const void *val, const OcType *kt, const OcType *vt) {
    OcMap *m = *slot;
    if (!m) { m = oc_alloc(sizeof(OcMap)); m->kt = kt; m->vt = vt; *slot = m; }
    uint64_t h = key_hash(kt, key);
    int64_t i = map_find(m, key, h);
    if (i >= 0) {
        /* Replacement reuses the bucket; the copy is made before releasing the old value. */
        char *old = m->vals + i * vt->size;
        if (owned(vt)) {
            char stack_tmp[256];
            size_t vsz = vt->size ? checked_i64_size(vt->size) : 1;
            char *tmp = vsz <= sizeof(stack_tmp) ? stack_tmp : xmalloc(vsz);
            oc_copy(tmp, val, vt);
            oc_destroy(old, vt);
            if (vt->size) memcpy(old, tmp, (size_t)vt->size);
            if (tmp != stack_tmp) free(tmp);
        } else oc_copy(old, val, vt);
        return;
    }
    if ((m->count + m->tombs) * 2 >= m->cap) map_grow(m); /* at most half full */
    int64_t j = (int64_t)(h & (uint64_t)(m->cap - 1));
    while (m->hashes[j] >= 2) j = (j + 1) & (m->cap - 1);
    if (m->hashes[j] == 1) m->tombs--;
    m->hashes[j] = h;
    oc_copy(m->keys + j * kt->size, key, kt);
    oc_copy(m->vals + j * vt->size, val, vt);
    m->count++;
}
/* Address of the value for key, inserting a zero value if missing (for m[k] op= v). */
void *oc_map_slot(OcMap **slot, const void *key, const OcType *kt, const OcType *vt) {
    void *p = oc_map_get(*slot, key, kt);
    if (p) return p;
    char stack_zero[256];
    size_t vsz = vt->size ? checked_i64_size(vt->size) : 1;
    char *zero = vsz <= sizeof(stack_zero) ? stack_zero : xmalloc(vsz);
    oc_zero(zero, vt);
    oc_map_set(slot, key, zero, kt, vt);
    if (zero != stack_zero) free(zero);
    return oc_map_get(*slot, key, kt);
}
void oc_map_delete(OcMap **slot, const void *key, const OcType *kt) {
    OcMap *m = *slot;
    if (!m) return;
    int64_t i = map_find(m, key, key_hash(kt, key));
    if (i < 0) return;
    m->hashes[i] = 1; m->tombs++; m->count--;
    oc_destroy(m->keys + i * m->kt->size, m->kt);
    oc_destroy(m->vals + i * m->vt->size, m->vt);
    memset(m->keys + i * m->kt->size, 0, (size_t)m->kt->size);
    memset(m->vals + i * m->vt->size, 0, (size_t)m->vt->size);
}
int64_t oc_map_len(OcMap *m) { return m ? m->count : 0; }
OcMap *oc_map_copy(OcMap *m) {
    if (!m || !m->count) return NULL;
    OcMap *r = NULL;
    for (int64_t i = 0; i < m->cap; i++)
        if (m->hashes[i] >= 2) oc_map_set(&r, m->keys + i * m->kt->size, m->vals + i * m->vt->size, m->kt, m->vt);
    return r;
}
int64_t oc_map_equal(OcMap *a, OcMap *b) {
    if (oc_map_len(a) != oc_map_len(b)) return 0;
    if (!oc_map_len(a)) return 1;
    for (int64_t i = 0; i < a->cap; i++) {
        if (a->hashes[i] < 2) continue;
        void *v = oc_map_get(b, a->keys + i * a->kt->size, a->kt);
        if (!v || !oc_equal(a->vals + i * a->vt->size, v, a->vt)) return 0;
    }
    return 1;
}
/* Snapshot for range: dynamic arrays of keys and values (deep copies). */
void oc_map_snapshot(OcMap *m, const OcType *kt, const OcType *vt, OcArr **keys, OcArr **vals) {
    *keys = NULL; *vals = NULL;
    if (!m || !m->count) return;
    *keys = arr_make(m->count, m->count, kt->size);
    *vals = arr_make(m->count, m->count, vt->size ? vt->size : 1);
    int64_t n = 0;
    for (int64_t i = 0; i < m->cap; i++) {
        if (m->hashes[i] < 2) continue;
        oc_copy((*keys)->data + n * kt->size, m->keys + i * kt->size, kt);
        oc_copy((*vals)->data + n * vt->size, m->vals + i * vt->size, vt);
        n++;
    }
}

/* ---------------- reflection ---------------- */
typedef struct { OcArr *name; OcAny value; } OcReflectField;
static void *field_block(int64_t n) {
    if (n < 0) __builtin_trap();
    int64_t payload = checked_i64_mul(n, (int64_t)sizeof(OcReflectField));
    char *r = oc_alloc(checked_i64_add(8, payload));
    *(int64_t *)r = payload;
    return r;
}
void *oc_reflect_fields(const OcType *t, const void *data) {
    if (!t) return field_block(0);
    if (t->kind == K_STRUCT) {
        OcReflectField *out = (OcReflectField *)((char *)field_block(t->nfields) + 8);
        for (int64_t i = 0; i < t->nfields; i++) {
            const OcField *f = &t->fields[i];
            out[i].name = oc_str_c(f->name);
            out[i].value.t = f->type;
            if (f->bits) {
                char stack_tmp[16] = {0};
                char *tmp = f->type->size > 16 ? xcalloc(1, checked_i64_size(f->type->size)) : stack_tmp;
                read_bits(data, f, tmp, f->type);
                out[i].value.data = oc_box(f->type, tmp);
                if (tmp != stack_tmp) free(tmp);
            }
            else if (f->type->kind == K_IFACE) oc_copy(&out[i].value, (const char *)data + f->offset, f->type);
            else out[i].value.data = oc_box(f->type, (const char *)data + f->offset);
        }
        return (char *)out - 8;
    }
    if (t->kind == K_ENUM) {
        int64_t tag = *(const int64_t *)data;
        if (tag < 0 || tag >= t->nvariants) return field_block(0);
        const OcVariant *v = &t->variants[tag];
        OcReflectField *out = (OcReflectField *)((char *)field_block(v->nfields) + 8);
        for (int64_t i = 0; i < v->nfields; i++) {
            const OcField *f = &v->fields[i];
            out[i].name = f->name && f->name[0] && !(f->name[0] >= '0' && f->name[0] <= '9') ? oc_str_c(f->name) : NULL;
            const char *p = (const char *)data + t->payloadOffset + f->offset;
            if (f->type->kind == K_IFACE) oc_copy(&out[i].value, p, f->type);
            else { out[i].value.t = f->type; out[i].value.data = oc_box(f->type, p); }
        }
        return (char *)out - 8;
    }
    return field_block(0);
}
int64_t oc_reflect_is_struct(const OcType *t) { return t && t->kind == K_STRUCT; }
int64_t oc_reflect_is_enum(const OcType *t) { return t && t->kind == K_ENUM; }
OcArr *oc_reflect_enum_variant(const OcType *t, const void *data) {
    if (!t || t->kind != K_ENUM) return NULL;
    int64_t tag = *(const int64_t *)data;
    if (tag < 0 || tag >= t->nvariants) return NULL;
    size_t a = strlen(t->name), b = strlen(t->variants[tag].name);
    char *s = xmalloc(checked_add(checked_add(a, b), 2));
    memcpy(s, t->name, a); s[a] = '.'; memcpy(s + a + 1, t->variants[tag].name, b); s[a + b + 1] = 0;
    OcArr *r = oc_str_c(s); free(s);
    return r;
}

/* Bytes of a boxed varint(n)/varuint(n) (also through a defined type): a header
 * byte holding the bit count of the top byte (+8 when signed), then the value's
 * little-endian bit pattern. Other values give an empty string. */
OcArr *oc_reflect_var_integer(const OcType *t, const void *data) {
    if (!t || t->kind != K_INT || !(t->flags & F_VARW) || !data) return arr_make(0, 0, 1);
    int64_t bits = t->bits, bytes = (bits + 7) / 8;
    OcArr *r = arr_make(bytes + 1, bytes + 1, 1);
    int64_t top = (bits - 1) % 8 + 1;
    r->data[0] = (char)(top + ((t->flags & F_SIGNED) ? 8 : 0));
    memcpy(r->data + 1, data, (size_t)bytes);
    if (top < 8) r->data[bytes] &= (char)((1 << top) - 1); /* drop padding bits */
    return r;
}

/* Elements of a boxed array as a dynamic array of boxes (mixed []any). */
OcArr *oc_box_elements(const OcType *t, const void *data) {
    const char *elems; int64_t n; const OcType *e = t->elem;
    if (t->kind == K_DYN) { if (!data) return NULL; const OcArr *a = *(OcArr *const *)data; n = alen(a); elems = a ? a->data : NULL; }
    else if (t->kind == K_ARRAY) { if (!data) return NULL; n = block_count(data, e); elems = (const char *)data + array_elems_offset(e); }
    else return NULL;
    if (!n) return NULL;
    OcArr *r = arr_make(n, n, sizeof(OcAny));
    OcAny *out = (OcAny *)r->data;
    for (int64_t i = 0; i < n; i++) {
        if (e->kind == K_IFACE) oc_copy(&out[i], elems + i * e->size, e);
        else { out[i].t = e; out[i].data = oc_box(e, elems + i * e->size); }
    }
    return r;
}
/* Element-wise conformance of a boxed array value: every element's dynamic
 * type passes m (when given) and, for homogeneous targets, all are the same. */
typedef _Bool (*OcMatcher)(const OcType *);
static int elems_match(const OcType *et, const char *elems, int64_t n, OcMatcher m, int64_t hom) {
    const OcType *first = NULL;
    for (int64_t i = 0; i < n; i++) {
        const OcType *dt = et->kind == K_IFACE ? ((const OcAny *)(elems + i * et->size))->t : et;
        if (!dt) continue;
        if (m && !m(dt)) return 0;
        if (hom && first && dt != first) return 0;
        if (!first) first = dt;
    }
    return 1;
}
_Bool oc_elems_match(const OcType *t, const void *data, OcMatcher m, int64_t hom) {
    if (!t) return 0;
    if (t->kind == K_DYN) { const OcArr *a = *(OcArr *const *)data; return elems_match(t->elem, a ? a->data : NULL, alen(a), m, hom); }
    if (t->kind == K_ARRAY) {
        const char *blk = data; /* a boxed runtime-sized array's data is its block */
        if (!blk) return 1;
        return elems_match(t->elem, blk + array_elems_offset(t->elem), block_count(blk, t->elem), m, hom);
    }
    return 0;
}
/* Converting an interface array: trap unless every element conforms. */
void oc_iface_elems_check(OcArr *a, OcMatcher m, int64_t hom) {
    if (!elems_match(&(OcType){.kind = K_IFACE, .size = sizeof(OcAny)}, a ? a->data : NULL, alen(a), m, hom)) __builtin_trap();
}

/* Fixed-array block of boxes for array<any>. */
void *oc_box_elements_block(const OcType *t, const void *data) {
    OcArr *a = oc_box_elements(t, data);
    int64_t n = alen(a);
    int64_t payload = checked_i64_mul(n, (int64_t)sizeof(OcAny));
    char *r = oc_alloc(checked_i64_add(8, payload));
    *(int64_t *)r = payload;
    if (n) memcpy(r + 8, a->data, (size_t)payload);
    if (a) free(a); /* elements were moved into the block */
    return r;
}

/* ---------------- interface dispatch ---------------- */
typedef struct { const OcType *t; const char *name; void *fn; } OcGenericMethod;
/* defined by every generated module */
extern const int64_t oc_generic_method_count;
extern const OcGenericMethod oc_generic_methods[];
void *oc_method(const OcType *t, const char *name, int64_t site) {
    if (t) {
        for (int64_t i = 0; i < oc_generic_method_count; i++)
            if (oc_generic_methods[i].t == t && !strcmp(oc_generic_methods[i].name, name)) return oc_generic_methods[i].fn;
    }
    if (!t) {
        OcArr *msg = oc_str_c("method call on null interface");
        oc_panic(msg, site);
    }
    for (int64_t i = 0; i < t->nmethods; i++) if (t->methods[i].name == name) return t->methods[i].fn;
    for (int64_t i = 0; i < t->nmethods; i++) if (!strcmp(t->methods[i].name, name)) return t->methods[i].fn;
    __builtin_trap();
}

/* ---------------- panics, defers, recover ---------------- */
typedef struct OcPanic {
    struct OcPanic *outer;
    OcArr *msg;
    char *header;
    size_t header_length;
    OcFrame *deferFrame;
    int recovered, active, returnPanic;
} OcPanic;
static __thread OcPanic *panicking;

static int is_strict_ancestor(const OcFrame *anc, const OcFrame *f) {
    if (!anc || !f) return 0;
    for (const OcFrame *c = f->prev; c; c = c->prev) if (c == anc) return 1;
    return 0;
}

static void release_defer(OcDefer *d) {
    d->thunk(d, 0);
    free(d);
}

void oc_run_defers(OcFrame *f) {
    while (f->defers) {
        OcDefer *d = f->defers;
        f->defers = d->next;
        d->next = f->active_defers;
        f->active_defers = d;
        oc_in_defer++;
        d->thunk(d, 1);
        oc_in_defer--;
        f->active_defers = d->next;
        release_defer(d);
    }
}

static void drop_locals(OcFrame *f) {
    if (f->locals) for (const OcLocal *l = f->locals; l->slot; l++) oc_drop(l->slot, l->t);
}
static void discard_defers(OcFrame *f) {
    while (f->defers) {
        OcDefer *d = f->defers;
        f->defers = d->next;
        release_defer(d);
    }
}
/* Called only when unwinding has reached the frame that invoked the defer.
 * A panic recovered inside the deferred call leaves its active record intact. */
static void abandon_active_defers(OcFrame *f) {
    while (f->active_defers) {
        OcDefer *d = f->active_defers;
        f->active_defers = d->next;
        oc_in_defer--;
        release_defer(d);
    }
}

extern void ocean_threads_panic_exit(int32_t status); /* threads.c or a module stub */
static void die(void) {
    if (ocean_threads_panic_exit) ocean_threads_panic_exit(1);
    exit(1);
}

static char *msg_text(OcArr *msg, size_t *length) {
    int64_t n = alen(msg);
    char *s = xmalloc(checked_add(checked_i64_size(n), 8));
    memcpy(s, "Panic: ", 7);
    if (n) memcpy(s + 7, msg->data, (size_t)n);
    s[7 + n] = 0;
    *length = (size_t)n + 7;
    return s;
}

/* Unwind from frame 'start', running defers; recovery resumes in the frame. */
void oc_drop(void *slot, const OcType *t);
/* Locals only need releasing when some outer frame may recover (otherwise the
 * process is about to exit). */
static int recovery_possible(OcFrame *f) {
    for (; f; f = f->prev) if (f->jb) return 1;
    return 0;
}
static void unwind(OcArr *msg, int64_t site, OcFrame *start, OcFrame *traceFrom) {
    OcPanic p = {0};
    p.header = msg_text(msg, &p.header_length);
    p.outer = panicking; p.msg = msg; p.deferFrame = start; p.active = 1;
    panicking = &p;
    for (OcFrame *f = start; f; f = f->prev) {
        oc_top = f;
        abandon_active_defers(f);
        if (f->defers) {
            p.deferFrame = f;
            oc_run_defers(f);
            if (p.recovered && f->jb) {
                extern const OcType oc_type_string;
                while (panicking && !is_strict_ancestor(panicking->deferFrame, f)) {
                    OcPanic *old = panicking;
                    panicking = old->outer;
                    if (old->msg) oc_drop(&old->msg, &oc_type_string);
                    free(old->header);
                    old->header = NULL;
                }
                discard_defers(f);
                oc_in_defer = f->in_defer;
                oc_top = f;
                _longjmp(*(jmp_buf *)f->jb, 1);
            }
        }
        /* the panic continues past this frame: release what it owns */
        if (f->locals && (p.recovered || recovery_possible(f->prev))) drop_locals(f);
    }
    oc_trace(p.header, p.header_length, site, traceFrom);
    die();
}
void oc_panic(OcArr *msg, int64_t site) {
    OcFrame *f = oc_top;
    unwind(msg, site, f, f);
}
_Noreturn void oc_panic_sync_closed(void) {
    oc_panic(oc_str_c("synchronization object is closed"), oc_top ? oc_top->site : 0);
    abort();
}
void oc_panic_div_zero(int64_t site) { oc_panic(oc_str_c("integer division by zero"), site); }
void oc_panic_neg_shift(int64_t site) { oc_panic(oc_str_c("negative shift count"), site); }
/* return panic: attribute to the caller's call site; skip this function's
 * defers and those of forwarding callers. */
void oc_return_panic(OcArr *msg, int64_t site) {
    OcFrame *f = oc_top;
    OcFrame *target = f && f->prev ? f->prev : f;
    if (!f || !f->prev || !f->prev->name) { unwind(msg, site, f, f); return; }
    while (target->prev && target->prev->name && (target->site & FORWARD_BIT)) target = target->prev;
    int can_rec = recovery_possible(target);
    for (OcFrame *s = f; s && s != target; s = s->prev) {
        abandon_active_defers(s);
        discard_defers(s);
        if (can_rec) drop_locals(s);
    }
    unwind(msg, target->site & ~FORWARD_BIT, target, f);
}
void oc_recover(OcAny *out) {
    out->t = NULL; out->data = NULL;
    OcPanic *p = panicking;
    if (!p || !p->active || p->recovered) return;
    if (!oc_top || oc_top->prev != p->deferFrame) return;
    p->recovered = 1;
    extern const OcType oc_type_string;
    out->t = &oc_type_string;
    OcArr **box = oc_alloc(8);
    *box = oc_str_copy(p->msg); /* the panic keeps (and later frees) its own */
    out->data = box;
}
int64_t oc_can_cancel(void) { return !oc_in_defer; }
void oc_uninit_func(int64_t site) { oc_panic(oc_str_c("call of an uninitialized function"), site); }
void oc_map_missing(int64_t site) { oc_panic(oc_str_c("map key does not exist"), site); }

/* Thread return/cancel: run every defer on this thread, then exit it. */
extern _Noreturn void ocean_threads_exit(void);
extern void ocean_threads_begin_return(void);
void ocean_runtime_thread_return(void) {
    if (ocean_threads_begin_return) ocean_threads_begin_return();
    for (OcFrame *f = oc_top; f; f = f->prev) {
        oc_top = f;
        abandon_active_defers(f);
        oc_run_defers(f);
        drop_locals(f);
    }
    oc_top = NULL;
    if (ocean_threads_exit) ocean_threads_exit();
    exit(0);
}
int ocean_runtime_can_cancel(void) { return !oc_in_defer; }
void ocean_runtime_closure_init(void) {}
void ocean_runtime_thread_invoke(void *code, void *environment) {
    oc_top = NULL;
    ((void (*)(void *))code)(environment);
}

/* ---------------- startup ---------------- */
#ifndef OC_LIBRARY
extern void oc_init_globals(void);
extern int64_t oc_user_main(void);
extern void oc_set_args(char *stream, int64_t argc);
int oc_argc; char **oc_argv;
int oc_start(void) {
    int argc = oc_argc; char **argv = oc_argv;
    if (!argv || argc <= 0) { static char *none[] = {"", NULL}; argv = none; argc = 1; }
    size_t total = 0;
    for (int i = 0; i < argc; i++) total = checked_add(total, checked_add(strlen(argv[i] ? argv[i] : ""), 1));
    char *stream = xmalloc(checked_add(total, 1)), *p = stream;
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i] ? argv[i] : "";
        size_t n = strlen(s) + 1;
        memcpy(p, s, n);
        p += n;
    }
    *p = 0;
    oc_set_args(stream, argc);
    free(stream);
    oc_init_globals();
    int64_t status = oc_user_main();
    return (int)status;
}
#endif
