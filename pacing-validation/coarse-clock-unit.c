/* Real fallback clock, forced non-monotonic platform configuration. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <assert.h>
#include "ngx_times.c"
int main(void) {
    ngx_usec_t first, now;
    ngx_current_msec = (ngx_msec_t) -3;
    first = ngx_monotonic_usec();
    ngx_current_msec = (ngx_msec_t) -1;
    now = ngx_monotonic_usec();
    assert(now - first == 2000);
    ngx_current_msec = 2;
    now = ngx_monotonic_usec();
    assert(now - first == 5000);
    ngx_current_msec = 1;
    assert(ngx_monotonic_usec() == now);
    ngx_current_msec = 3;
    assert(ngx_monotonic_usec() == now + 2000);
    assert(ngx_monotonic_usec() == now + 2000);
    printf("PASS: coarse clock wrap/regression, %zu-bit millisecond counter\n",
           sizeof(ngx_msec_t) * 8);
    return 0;
}
