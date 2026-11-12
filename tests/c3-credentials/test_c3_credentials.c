#include "net_link_credentials.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char ssid[34], pass[66];
    memset(ssid, 's', 32); ssid[32] = 0;
    memset(pass, 'a', 64); pass[64] = 0;
    assert(net_link_credentials_valid(ssid, pass));
    assert(net_link_credentials_valid("open", ""));
    assert(net_link_credentials_valid("ap", "12345678"));
    assert(!net_link_credentials_valid("", "12345678"));
    ssid[32] = 's'; ssid[33] = 0;
    assert(!net_link_credentials_valid(ssid, "12345678"));
    assert(!net_link_credentials_valid("ap", "1234567"));
    pass[20] = 'z';
    assert(!net_link_credentials_valid("ap", pass));
    pass[20] = 'a'; pass[64] = 'a'; pass[65] = 0;
    assert(!net_link_credentials_valid("ap", pass));
    puts("c3 credentials malformed/boundary inputs: PASS");
    return 0;
}
