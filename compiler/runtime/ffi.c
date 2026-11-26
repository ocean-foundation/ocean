/* C variadic calls whose argument count is known only at run time (array or
 * ...any expansion). Linked only into programs that need it; requires libffi. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ffi.h>

typedef struct OcType OcType;
struct OcType { int64_t kind, size, align, bits, flags; const OcType *elem, *key; const char *name; };
typedef struct { const OcType *t; void *data; } OcAny;
typedef struct OcArr { int64_t len, cap; char *data; } OcArr;
enum { K_INT = 1, K_FLOAT, K_BOOL, K_STRING, K_PTR, K_DYN, K_ARRAY, K_BUFFER, K_MAP, K_STRUCT, K_ENUM, K_IFACE, K_FUNC, K_BUFLIST };
/* slot kinds: 1 i32, 2 i64, 3 double, 4 float, 5 pointer, 6 i8, 7 i16, 0 void */
static ffi_type *slot_type(int64_t k) {
    switch (k) {
    case 1: return &ffi_type_sint32;
    case 2: return &ffi_type_sint64;
    case 3: return &ffi_type_double;
    case 4: return &ffi_type_float;
    case 5: return &ffi_type_pointer;
    case 6: return &ffi_type_sint8;
    case 7: return &ffi_type_sint16;
    }
    return &ffi_type_void;
}
extern void *oc_str_cdata(OcArr *a);
static void *ffi_calloc(size_t count, size_t sz) {
    size_t tot;
    if (__builtin_mul_overflow(count, sz, &tot)) __builtin_trap();
    void *p = calloc(1, tot ? tot : 1);
    if (!p) __builtin_trap();
    return p;
}
static void store_slot(int64_t kind, uint64_t raw, uint64_t *dst) {
    *dst = 0;
    if (kind == 4) {
        uint32_t bits = (uint32_t)raw;
        float fv;
        memcpy(&fv, &bits, 4);
        memcpy(dst, &fv, sizeof(fv));
    } else if (kind == 1) {
        int32_t iv = (int32_t)raw;
        memcpy(dst, &iv, sizeof(iv));
    } else if (kind == 6) {
        int8_t iv = (int8_t)raw;
        memcpy(dst, &iv, sizeof(iv));
    } else if (kind == 7) {
        int16_t iv = (int16_t)raw;
        memcpy(dst, &iv, sizeof(iv));
    } else if (kind == 5) {
        void *p = (void *)(uintptr_t)raw;
        memcpy(dst, &p, sizeof(p));
    } else {
        *dst = raw;
    }
}
/* Classify a boxed value with C default argument promotions. */
static void promote(const OcAny *a, int64_t *kind, uint64_t *value) {
    const OcType *t = a->t;
    *kind = 2; *value = 0;
    if (!t) { *kind = 5; return; }
    if (t->kind == K_BUFFER && t->bits <= 0) {
        *kind = 5;
        store_slot(5, (uint64_t)(uintptr_t)a->data, value);
        return;
    }
    if (!a->data) { *kind = 5; return; }
    if (t->kind == K_INT || t->kind == K_BOOL) {
        int64_t bits = t->kind == K_BOOL ? 1 : t->bits;
        uint64_t raw = 0;
        size_t sz = (t->size > 0 && t->size < 8) ? (size_t)t->size : (t->size >= 8 ? 8 : 0);
        if (sz) memcpy(&raw, a->data, sz);
        if (bits < 64) raw &= ((uint64_t)1 << bits) - 1;
        int sign = (t->flags & 1) && bits < 64 && ((raw >> (bits - 1)) & 1);
        if (sign) raw |= ~(((uint64_t)1 << bits) - 1);
        *kind = bits <= 32 ? 1 : 2;
        store_slot(*kind, raw, value);
        return;
    }
    if (t->kind == K_FLOAT) {
        double d = t->bits == 32 ? *(const float *)a->data : *(const double *)a->data;
        memcpy(value, &d, 8); *kind = 3; return;
    }
    if (t->kind == K_STRING) {
        *kind = 5;
        store_slot(5, (uint64_t)(uintptr_t)oc_str_cdata(*(OcArr *const *)a->data), value);
        return;
    }
    *kind = 5;
    if (t->kind == K_PTR || t->kind == K_FUNC) {
        size_t sz = (t->size > 0 && t->size < 8) ? (size_t)t->size : (t->size >= 8 ? 8 : 0);
        if (sz) memcpy(value, a->data, sz);
    } else {
        store_slot(5, (uint64_t)(uintptr_t)a->data, value);
    }
}
/* fn: target; ret: slot kind of the result; n fixed+explicit slots in kinds/values,
 * nfixed of them declared; extras: dynamic array of OcAny appended. */
uint64_t oc_cvar_call(void *fn, int64_t ret, int64_t nfixed, int64_t n, const int64_t *kinds, const uint64_t *values, OcArr *extras) {
    if (n < 0 || nfixed < 0) __builtin_trap();
    int64_t nextra = extras ? extras->len : 0;
    if (nextra < 0) __builtin_trap();
    int64_t total;
    if (__builtin_add_overflow(n, nextra, &total) || total < 0 || (uint64_t)total > (SIZE_MAX / sizeof(uint64_t)) - 1) __builtin_trap();
    size_t cap = (size_t)total + 1;
    ffi_type **types = ffi_calloc(cap, sizeof(*types));
    void **args = ffi_calloc(cap, sizeof(*args));
    uint64_t *store = ffi_calloc(cap, sizeof(*store));
    int64_t *k = ffi_calloc(cap, sizeof(*k));
    for (int64_t i = 0; i < n; i++) { k[i] = kinds[i]; store_slot(k[i], values[i], &store[i]); }
    for (int64_t i = 0; i < nextra; i++) promote(&((OcAny *)extras->data)[i], &k[n + i], &store[n + i]);
    for (int64_t i = 0; i < total; i++) { types[i] = slot_type(k[i]); args[i] = &store[i]; }
    ffi_cif cif;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, (unsigned)nfixed, (unsigned)total, slot_type(ret), types) != FFI_OK) abort();
    uint64_t out = 0;
    if (ret == 3) {
        double dres = 0.0;
        ffi_call(&cif, FFI_FN(fn), &dres, args);
        memcpy(&out, &dres, 8);
    } else if (ret == 4) {
        float fres = 0.0f;
        ffi_call(&cif, FFI_FN(fn), &fres, args);
        uint32_t bits = 0;
        memcpy(&bits, &fres, 4);
        out = (uint64_t)bits;
    } else if (ret == 5) {
        void *pres = NULL;
        ffi_call(&cif, FFI_FN(fn), &pres, args);
        out = (uint64_t)(uintptr_t)pres;
    } else {
        ffi_arg ares = 0;
        ffi_call(&cif, FFI_FN(fn), ret ? (void *)&ares : NULL, args);
        if (ret == 6) out = (uint64_t)(int64_t)(int8_t)ares;
        else if (ret == 7) out = (uint64_t)(int64_t)(int16_t)ares;
        else if (ret == 1) out = (uint64_t)(int64_t)(int32_t)ares;
        else out = (uint64_t)ares;
    }
    free(types); free(args); free(store); free(k);
    return out;
}
