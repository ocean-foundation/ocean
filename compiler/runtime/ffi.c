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
enum { K_INT = 1, K_FLOAT, K_BOOL, K_STRING, K_PTR, K_DYN, K_ARRAY, K_BUFFER };
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
/* Classify a boxed value with C default argument promotions. */
static void promote(const OcAny *a, int64_t *kind, uint64_t *value) {
    const OcType *t = a->t;
    *kind = 2; *value = 0;
    if (!t) { *kind = 5; return; }
    if (t->kind == K_INT || t->kind == K_BOOL) {
        int64_t bits = t->kind == K_BOOL ? 1 : t->bits;
        uint64_t raw = 0;
        memcpy(&raw, a->data, (size_t)(t->size > 8 ? 8 : t->size));
        if (bits < 64) raw &= ((uint64_t)1 << bits) - 1;
        int sign = (t->flags & 1) && bits < 64 && ((raw >> (bits - 1)) & 1);
        if (sign) raw |= ~(((uint64_t)1 << bits) - 1);
        *kind = bits <= 32 ? 1 : 2;
        *value = raw;
        return;
    }
    if (t->kind == K_FLOAT) {
        double d = t->bits == 32 ? *(float *)a->data : *(double *)a->data;
        memcpy(value, &d, 8); *kind = 3; return;
    }
    if (t->kind == K_STRING) { *kind = 5; *value = (uint64_t)(uintptr_t)oc_str_cdata(*(OcArr **)a->data); return; }
    if (t->kind == K_BUFFER) { *kind = 5; *value = (uint64_t)(uintptr_t)a->data; return; }
    *kind = 5;
    memcpy(value, a->data, 8);
}
/* fn: target; ret: slot kind of the result; n fixed+explicit slots in kinds/values,
 * nfixed of them declared; extras: dynamic array of OcAny appended. */
uint64_t oc_cvar_call(void *fn, int64_t ret, int64_t nfixed, int64_t n, const int64_t *kinds, const uint64_t *values, OcArr *extras) {
    int64_t nextra = extras ? extras->len : 0, total = n + nextra;
    ffi_type **types = calloc((size_t)total + 1, sizeof(*types));
    void **args = calloc((size_t)total + 1, sizeof(*args));
    uint64_t *store = calloc((size_t)total + 1, sizeof(*store));
    int64_t *k = calloc((size_t)total + 1, sizeof(*k));
    for (int64_t i = 0; i < n; i++) { k[i] = kinds[i]; store[i] = values[i]; }
    for (int64_t i = 0; i < nextra; i++) promote(&((OcAny *)extras->data)[i], &k[n + i], &store[n + i]);
    for (int64_t i = 0; i < total; i++) { types[i] = slot_type(k[i]); args[i] = &store[i]; }
    ffi_cif cif;
    uint64_t result = 0;
    if (ffi_prep_cif_var(&cif, FFI_DEFAULT_ABI, (unsigned)nfixed, (unsigned)total, slot_type(ret), types) != FFI_OK) abort();
    ffi_call(&cif, FFI_FN(fn), &result, args);
    free(types); free(args); free(store); free(k);
    return result;
}
