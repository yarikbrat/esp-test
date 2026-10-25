#pragma once
// Мінімальна тестова обв'язка без залежностей.
#include <math.h>
#include <stdio.h>

extern int g_checks, g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(cond)) {                                                           \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                    \
    do {                                                                         \
        double a_ = (a), b_ = (b);                                               \
        g_checks++;                                                              \
        if (!(fabs(a_ - b_) <= (eps))) {                                         \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: %s = %.6f, expected %.6f ± %g\n", __FILE__, __LINE__, \
                    #a, a_, b_, (double)(eps));                                  \
        }                                                                        \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                       \
    do {                                                                         \
        long long a_ = (long long)(a), b_ = (long long)(b);                      \
        g_checks++;                                                              \
        if (a_ != b_) {                                                          \
            g_failures++;                                                        \
            fprintf(stderr, "%s:%d: %s = %lld, expected %lld\n", __FILE__, __LINE__, #a, a_, b_); \
        }                                                                        \
    } while (0)

void test_altitude(void);
void test_zero_cal(void);
void test_netcfg(void);
void test_http(void);
void test_http_fuzz(void);
void test_form(void);
void test_histlog(void);
void test_settings(void);
void test_stand(void);
void test_baro_average(void);
void test_jsonw(void);
void test_api(void);
