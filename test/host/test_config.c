/*
 * test_config.c — Dual-Pool Proxy tests for JSON config parsing (our config) and
 * ckproxy config emission. Requires jansson (run in the dualpool-dev image).
 * GPLv3.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <jansson.h>
#include "config.h"
#include "ckproxy_config.h"

static const char *FULL =
"{"
"  \"downstream\": { \"stratum_port\": 3333, \"web_port\": 8080 },"
"  \"mode\": \"farm_split\", \"ratio_a\": 70, \"interval_ms\": 5000,"
"  \"web_password\": \"secret\","
"  \"pools\": ["
"    { \"url\": \"poolA:3333\", \"user\": \"walletA.w1\", \"pass\": \"x\","
"      \"ckproxy_mode\": \"userproxy\","
"      \"failover\": { \"url\": \"backupA:3333\", \"user\": \"walletA.w1\", \"pass\": \"y\" } },"
"    { \"url\": \"poolB:3333\", \"user\": \"walletB.w1\", \"pass\": \"z\" }"
"  ]"
"}";

static void test_parse_full(void) {
    dualpool_config_t c;
    char err[256];
    assert(config_parse_string(FULL, &c, err, sizeof(err)) == 0);
    assert(c.stratum_port == 3333);
    assert(c.web_port == 8080);
    assert(c.ratio_a == 70);
    assert(c.interval_ms == 5000);
    assert(strcmp(c.mode, "farm_split") == 0);
    assert(strcmp(c.web_password, "secret") == 0);
    assert(strcmp(c.pools[0].primary.url, "poolA:3333") == 0);
    assert(strcmp(c.pools[0].primary.user, "walletA.w1") == 0);
    assert(strcmp(c.pools[0].ckproxy_mode, "userproxy") == 0);
    assert(c.pools[0].has_failover == true);
    assert(strcmp(c.pools[0].failover.url, "backupA:3333") == 0);
    assert(strcmp(c.pools[0].failover.pass, "y") == 0);
    assert(c.pools[1].has_failover == false);
    assert(strcmp(c.pools[1].primary.pass, "z") == 0);
}

/* Optional fields get sane defaults. */
static void test_defaults(void) {
    const char *min =
        "{ \"ratio_a\": 50, \"pools\": ["
        "  { \"url\": \"a:1\", \"user\": \"ua\", \"pass\": \"pa\" },"
        "  { \"url\": \"b:2\", \"user\": \"ub\", \"pass\": \"pb\" } ] }";
    dualpool_config_t c; char err[256];
    assert(config_parse_string(min, &c, err, sizeof(err)) == 0);
    assert(c.stratum_port == 3333);          /* default */
    assert(c.web_port == 8080);              /* default */
    assert(strcmp(c.mode, "farm_split") == 0);
    assert(c.interval_ms == 180000);
    assert(strcmp(c.pools[0].ckproxy_mode, "userproxy") == 0);   /* default */
}

/* ratio is clamped into [0,100]. */
static void test_ratio_clamped(void) {
    const char *j =
        "{ \"ratio_a\": 150, \"pools\": ["
        "  {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "  {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    dualpool_config_t c; char err[256];
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.ratio_a == 100);
}

/* Fewer than two pools is an error. */
static void test_requires_two_pools(void) {
    const char *j =
        "{ \"ratio_a\": 50, \"pools\": [ {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    dualpool_config_t c; char err[256];
    assert(config_parse_string(j, &c, err, sizeof(err)) != 0);
}

/* ckproxy_config emits a valid ckpool proxy config with primary+failover in the
 * proxy array and the local serverurl the splitter will connect to. */
static void test_ckproxy_emit(void) {
    dualpool_config_t c; char err[256];
    assert(config_parse_string(FULL, &c, err, sizeof(err)) == 0);

    char path[] = "/tmp/dualpool_ckproxyA.json";
    assert(ckproxy_config_write(&c.pools[0], 4001, "/tmp/sockA", path,
                                err, sizeof(err)) == 0);

    json_error_t je;
    json_t *root = json_load_file(path, 0, &je);
    assert(root);
    json_t *proxy = json_object_get(root, "proxy");
    assert(json_is_array(proxy) && json_array_size(proxy) == 2);  /* primary+failover */

    json_t *p0 = json_array_get(proxy, 0);
    assert(strcmp(json_string_value(json_object_get(p0, "url")), "poolA:3333") == 0);
    assert(strcmp(json_string_value(json_object_get(p0, "auth")), "walletA.w1") == 0);
    assert(strcmp(json_string_value(json_object_get(p0, "pass")), "x") == 0);

    json_t *p1 = json_array_get(proxy, 1);
    assert(strcmp(json_string_value(json_object_get(p1, "url")), "backupA:3333") == 0);

    json_t *surl = json_object_get(root, "serverurl");
    assert(json_is_array(surl) && json_array_size(surl) >= 1);
    assert(strstr(json_string_value(json_array_get(surl, 0)), "4001") != NULL);

    json_decref(root);
}

int main(void) {
    test_parse_full();
    test_defaults();
    printf("config: parse full + defaults passed\n");
    test_ratio_clamped();
    test_requires_two_pools();
    printf("config: clamp + validation passed\n");
    test_ckproxy_emit();
    printf("config: ckproxy emit passed\n");
    printf("ALL config host tests passed\n");
    return 0;
}
