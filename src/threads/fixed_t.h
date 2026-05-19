#ifndef THREADS_FIXED_POINT_H
#define THREADS_FIXED_POINT_H
#include <stdint.h>

typedef int32_t fixed_t; //define o tipo fixed_t
#define F (1 << 16) //reserva 16 bits para a parte fracionária

//conversão de inteiro para fixed
static inline fixed_t int_to_fixed(int n) {
    return n * F;
}

//conversão de fixed para inteiro (truncando)
static inline int fixed_to_int_zero(fixed_t x) {
    return x / F;
}

//conversão de fixed para inteiro (arredondando)
static inline int fixed_to_int_round(fixed_t x) {
    if (x >= 0) {
        return (x + (F / 2)) / F;
    } else {
        return (x - (F / 2)) / F;
    }
}

//adição de 2 fixed
static inline fixed_t add_fixed(fixed_t x, fixed_t y) {
    return x + y;
}

//subtração de 2 fixed
static inline fixed_t sub_fixed(fixed_t x, fixed_t y) {
    return x - y;
}

//adição de fixed com int
static inline fixed_t add_fixed_int(fixed_t x, int n) {
    return x + (n * F);
}

//subtração de fixed com int
static inline fixed_t sub_fixed_int(fixed_t x, int n) {
    return x - (n * F);
}

//multiplicação de 2 fixed
static inline fixed_t mult_fixed(fixed_t x, fixed_t y) {
    return ((int64_t) x) * y / F;
}

//multiplicação de fixed com int
static inline fixed_t mult_fixed_int(fixed_t x, int n) {
    return x * n;
}

//divisão de 2 fixed
static inline fixed_t div_fixed(fixed_t x, fixed_t y) {
    return (((int64_t) x) * F) / y; //uso de int64 para evitar overflow
}

//divisão fixed por int
static inline fixed_t div_fixed_int(fixed_t x, int n) {
    return x / n;
}

#endif /* threads/fixed-point.h */
