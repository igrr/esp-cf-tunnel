#include <string.h>

#include "cft_conn.h"
#include "cft_ca.h"
#include "cft_util.h"

const char *const cft_edge_ca_pem_str = cft_edge_ca_pem;

/* Token: base64 of {"a": account tag, "t": tunnel UUID, "s": base64 secret}
 * (cloudflared connection.TunnelToken). */
bool cft_token_parse(const char *token, cft_token_t *out)
{
    char json[512];
    char tunnel_id[40];
    char secret_b64[128];
    memset(out, 0, sizeof(*out));
    int n = cft_b64_decode(token, strlen(token), (uint8_t *)json, sizeof(json));
    if (n <= 0) {
        return false;
    }
    if (!cft_json_get_string(json, (size_t)n, "a", out->account_tag, sizeof(out->account_tag)) ||
            !cft_json_get_string(json, (size_t)n, "t", tunnel_id, sizeof(tunnel_id)) ||
            !cft_json_get_string(json, (size_t)n, "s", secret_b64, sizeof(secret_b64))) {
        return false;
    }
    if (!cft_uuid_parse(tunnel_id, out->tunnel_id)) {
        return false;
    }
    int sl = cft_b64_decode(secret_b64, strlen(secret_b64), out->secret, sizeof(out->secret));
    if (sl <= 0) {
        return false;
    }
    out->secret_len = (size_t)sl;
    return true;
}
