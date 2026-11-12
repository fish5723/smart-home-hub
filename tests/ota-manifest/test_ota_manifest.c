#include "ota_manifest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static bool accepts(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    image_meta_t meta = {0};
    bool ok = root && ota_parse_image_meta(root, "p4", &meta);
    cJSON_Delete(root);
    return ok;
}

int main(void)
{
    char sha[65];
    memset(sha, 'a', 64); sha[64] = 0;
    char json[512];
#define META(V, S) snprintf(json, sizeof(json), "{\"p4\":{\"version\":\"%s\",\"url\":\"https://example.test/a.bin\",\"size\":%s,\"sha256\":\"%s\"}}", V, S, sha)
    META("0.8.2-netota", "1024"); assert(accepts(json));
    META("0.8.2", "0"); assert(!accepts(json));
    META("0.8.2", "-1"); assert(!accepts(json));
    META("0.8.2", "1.5"); assert(!accepts(json));
    META("0.8.2", "4294967296"); assert(!accepts(json));
    META("0.8.2", "1e309"); assert(!accepts(json));
    META("0.8.2", "null"); assert(!accepts(json));
    META("0.8", "100"); assert(!accepts(json));
    META("0.8.2evil", "100"); assert(!accepts(json));
    META("4294967296.8.2", "100"); assert(!accepts(json));
    META("0.8.2", "100"); sha[0] = 'z'; META("0.8.2", "100"); assert(!accepts(json));
    sha[0] = 'a';
    assert(!accepts("{\"p4\":[]}"));
    assert(ota_version_is_newer("0.8.2-netota", "0.8.1-netota"));
    assert(!ota_version_is_newer("0.8.1", "0.8.1-netota"));
    assert(!ota_version_is_newer("99999999999.0.0", "0.8.1"));
    puts("ota malformed/boundary inputs: PASS");
    return 0;
}
