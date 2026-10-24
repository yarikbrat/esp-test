#include "check.h"

int g_checks, g_failures;

int main(void)
{
    test_altitude();
    test_zero_cal();
    test_netcfg();
    test_http();
    test_http_fuzz();
    test_form();
    test_histlog();
    test_settings();
    test_stand();
    test_baro_average();
    test_jsonw();
    test_api();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
