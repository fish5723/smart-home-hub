#include "wifi_credentials.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    uint8_t ssid[32], password[64];
    bool secured = true;
    char s32[33], s33[34], p64[65], p65[66];
    memset(s32, 'S', 32); s32[32] = 0;
    memset(s33, 'S', 33); s33[33] = 0;
    memset(p64, 'a', 64); p64[64] = 0;
    memset(p65, 'a', 65); p65[65] = 0;
    assert(wifi_prepare_credentials(s32, p64, ssid, password, &secured));
    assert(secured && memcmp(ssid, s32, 32) == 0 && memcmp(password, p64, 64) == 0);
    assert(wifi_prepare_credentials("open", "", ssid, password, &secured) && !secured);
    assert(wifi_prepare_credentials("ap", "12345678", ssid, password, &secured) && secured);
    assert(!wifi_prepare_credentials("", "abcdefgh", ssid, password, &secured));
    assert(!wifi_prepare_credentials(s33, "abcdefgh", ssid, password, &secured));
    assert(!wifi_prepare_credentials("ap", "short", ssid, password, &secured));
    assert(!wifi_prepare_credentials("ap", p65, ssid, password, &secured));
    p64[20] = 'z';
    assert(!wifi_prepare_credentials("ap", p64, ssid, password, &secured));
    puts("wifi malformed/boundary inputs: PASS");
    return 0;
}
