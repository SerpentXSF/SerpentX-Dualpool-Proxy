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
"      \"ckproxy_mode\": \"userproxy\", \"startdiff\": 100000, \"mindiff\": 50000,"
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
    /* per-pool difficulty: set on A, unset (0 => default) on B */
    assert(c.pools[0].startdiff == 100000);
    assert(c.pools[0].mindiff == 50000);
    assert(c.pools[1].startdiff == 0);
    assert(c.pools[1].mindiff == 0);
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

/* hashrate_split mode is accepted and its slice knobs parse (with defaults). */
static void test_hashrate_split(void) {
    const char *j =
        "{ \"mode\": \"hashrate_split\", \"target_shares\": 8, \"ratio_a\": 50,"
        "  \"pools\": ["
        "    {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "    {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    dualpool_config_t c; char err[256];
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    /* mode is retained (not coerced back to farm_split) */
    assert(strcmp(c.mode, "hashrate_split") == 0);
    assert(c.target_shares == 8);
    /* absent knobs default to 10 / 10 / 120 */
    assert(c.min_slice_s == 10);
    assert(c.max_slice_s == 120);

    /* and when the knobs are entirely absent, target_shares also defaults to 10 */
    const char *d =
        "{ \"mode\": \"hashrate_split\", \"pools\": ["
        "    {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "    {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    assert(config_parse_string(d, &c, err, sizeof(err)) == 0);
    assert(strcmp(c.mode, "hashrate_split") == 0);
    assert(c.target_shares == 10);
    assert(c.min_slice_s == 10);
    assert(c.max_slice_s == 120);
}

/* EXPERIMENTAL assume_extranonce opt-in: absent => false (today's reconnect-slice
 * behaviour), explicit true => true, and a non-boolean value stays false so a
 * typo cannot silently enable it. */
static void test_assume_extranonce(void) {
    const char *pools =
        "  \"pools\": ["
        "    {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "    {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    char j[512];
    dualpool_config_t c; char err[256];

    /* absent -> false (default OFF) */
    snprintf(j, sizeof(j), "{ \"mode\": \"hashrate_split\",%s", pools);
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);

    /* explicit true -> true */
    snprintf(j, sizeof(j),
             "{ \"mode\": \"hashrate_split\", \"assume_extranonce\": true,%s", pools);
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == true);

    /* explicit false -> false */
    snprintf(j, sizeof(j),
             "{ \"mode\": \"hashrate_split\", \"assume_extranonce\": false,%s", pools);
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);

    /* non-boolean (a string) -> stays false, never coerced on */
    snprintf(j, sizeof(j),
             "{ \"mode\": \"hashrate_split\", \"assume_extranonce\": \"true\",%s", pools);
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);

    /* and the FULL farm_split config (which never mentions it) is unaffected */
    assert(config_parse_string(FULL, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);
}

/* ---------------------------------------------------------------------------
 * UPGRADE COMPATIBILITY (X.3.5 -> this release).
 *
 * An existing user's config predates target_shares / min_slice_s / max_slice_s /
 * assume_extranonce and the hashrate_split mode entirely. Dropping the new
 * binary on top of it must be a no-op: every field the old release honoured
 * keeps its exact value, and the new fields take their DOCUMENTED defaults
 * (10 shares / 10 s / 120 s / off) — never a value that would change how the
 * proxy already behaves for that user.
 * ------------------------------------------------------------------------ */

/* A verbatim X.3.5-era config: mode/ratio/interval/web_password/downstream plus
 * two fully-specified pools (url/user/pass/ckproxy_mode/failover/startdiff/
 * mindiff). Deliberately mentions NONE of the new keys. */
#define X35_POOLS \
"  \"pools\": ["                                                             \
"    { \"url\": \"eu.stratum.example:3333\", \"user\": \"bc1qold.rig1\","    \
"      \"pass\": \"oldpass\", \"ckproxy_mode\": \"proxy\","                  \
"      \"startdiff\": 8192, \"mindiff\": 1024,"                              \
"      \"failover\": { \"url\": \"eu2.stratum.example:3333\","               \
"                    \"user\": \"bc1qold.rig1\", \"pass\": \"fb\" } },"      \
"    { \"url\": \"solo.example:3333\", \"user\": \"bc1qsolo.rig1\","         \
"      \"pass\": \"x\", \"ckproxy_mode\": \"userproxy\","                    \
"      \"startdiff\": 100000, \"mindiff\": 50000 }"                          \
"  ]"

static const char *X35_FARM_SPLIT =
"{"
"  \"downstream\": { \"stratum_port\": 3355, \"web_port\": 8099 },"
"  \"mode\": \"farm_split\", \"ratio_a\": 65, \"interval_ms\": 240000,"
"  \"web_password\": \"hunter2\","
X35_POOLS
"}";

static const char *X35_TIME_SLICE =
"{"
"  \"downstream\": { \"stratum_port\": 3355, \"web_port\": 8099 },"
"  \"mode\": \"time_slice\", \"ratio_a\": 65, \"interval_ms\": 240000,"
"  \"web_password\": \"hunter2\","
X35_POOLS
"}";

/* Every pre-existing field survives an X.3.5 config byte-for-byte. */
static void check_x35_preexisting(const dualpool_config_t *c, const char *mode)
{
    assert(c->stratum_port == 3355);
    assert(c->web_port == 8099);
    assert(strcmp(c->mode, mode) == 0);          /* NOT coerced to another mode */
    assert(c->ratio_a == 65);                    /* in range: unclamped */
    assert(c->interval_ms == 240000);            /* in range: unclamped */
    assert(strcmp(c->web_password, "hunter2") == 0);

    assert(strcmp(c->pools[0].primary.url, "eu.stratum.example:3333") == 0);
    assert(strcmp(c->pools[0].primary.user, "bc1qold.rig1") == 0);
    assert(strcmp(c->pools[0].primary.pass, "oldpass") == 0);
    assert(strcmp(c->pools[0].ckproxy_mode, "proxy") == 0);   /* kept, not defaulted */
    assert(c->pools[0].startdiff == 8192);
    assert(c->pools[0].mindiff == 1024);
    assert(c->pools[0].has_failover == true);
    assert(strcmp(c->pools[0].failover.url, "eu2.stratum.example:3333") == 0);
    assert(strcmp(c->pools[0].failover.user, "bc1qold.rig1") == 0);
    assert(strcmp(c->pools[0].failover.pass, "fb") == 0);

    assert(strcmp(c->pools[1].primary.url, "solo.example:3333") == 0);
    assert(strcmp(c->pools[1].primary.user, "bc1qsolo.rig1") == 0);
    assert(strcmp(c->pools[1].primary.pass, "x") == 0);
    assert(strcmp(c->pools[1].ckproxy_mode, "userproxy") == 0);
    assert(c->pools[1].startdiff == 100000);
    assert(c->pools[1].mindiff == 50000);
    assert(c->pools[1].has_failover == false);
}

/* The new keys land on their documented defaults: 10 / 10 / 120 / false. */
static void check_new_defaults(const dualpool_config_t *c)
{
    assert(c->target_shares == 10);
    assert(c->min_slice_s == 10);
    assert(c->max_slice_s == 120);
    assert(c->min_slice_s <= c->max_slice_s);
    assert(c->assume_extranonce == false);
}

static void test_x35_upgrade(void)
{
    dualpool_config_t c; char err[256];

    /* farm_split — the mode the shipped default (and the author's own rigs) use */
    err[0] = '\0';
    assert(config_parse_string(X35_FARM_SPLIT, &c, err, sizeof(err)) == 0);
    check_x35_preexisting(&c, "farm_split");
    check_new_defaults(&c);

    /* time_slice — the other pre-existing mode */
    err[0] = '\0';
    assert(config_parse_string(X35_TIME_SLICE, &c, err, sizeof(err)) == 0);
    check_x35_preexisting(&c, "time_slice");
    check_new_defaults(&c);
}

/* An X.3.5 config cannot be talked into the experimental smooth-swap path by a
 * typo: only a real JSON `true` sets assume_extranonce. The string "true" (the
 * likely hand-edit) and a truthy number must both leave it OFF, because a miner
 * that ignores mining.set_extranonce would silently mine the wrong extranonce1
 * and have every share rejected. */
static void test_x35_assume_extranonce_strict(void)
{
    dualpool_config_t c; char err[256];
    char j[1400];

    /* the untouched X.3.5 configs never mention it -> OFF */
    assert(config_parse_string(X35_FARM_SPLIT, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);
    assert(config_parse_string(X35_TIME_SLICE, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);

    /* string "true" -> still OFF (bool_or is strict) */
    snprintf(j, sizeof(j),
             "{ \"mode\": \"farm_split\", \"assume_extranonce\": \"true\","
             X35_POOLS " }");
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);

    /* string "false", number 1, and null are all non-boolean -> OFF */
    snprintf(j, sizeof(j),
             "{ \"mode\": \"farm_split\", \"assume_extranonce\": \"false\","
             X35_POOLS " }");
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);
    snprintf(j, sizeof(j),
             "{ \"mode\": \"farm_split\", \"assume_extranonce\": 1,"
             X35_POOLS " }");
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);
    snprintf(j, sizeof(j),
             "{ \"mode\": \"farm_split\", \"assume_extranonce\": null,"
             X35_POOLS " }");
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == false);

    /* ONLY a real JSON true turns it on — and even then the X.3.5 fields and the
     * other new defaults are untouched. */
    snprintf(j, sizeof(j),
             "{ \"downstream\": { \"stratum_port\": 3355, \"web_port\": 8099 },"
             "  \"mode\": \"farm_split\", \"ratio_a\": 65, \"interval_ms\": 240000,"
             "  \"web_password\": \"hunter2\", \"assume_extranonce\": true,"
             X35_POOLS " }");
    assert(config_parse_string(j, &c, err, sizeof(err)) == 0);
    assert(c.assume_extranonce == true);
    check_x35_preexisting(&c, "farm_split");
    assert(c.target_shares == 10);
    assert(c.min_slice_s == 10);
    assert(c.max_slice_s == 120);
}

/* D4: the hashrate_split slice knobs are clamped and min<=max is enforced. */
static void test_slice_knobs_clamped(void) {
    /* over-range + inverted: target too big, min too big, max = 0 (the churn
     * trigger). Expect target->1000, min->3600, max raised to >= min. */
    const char *hi =
        "{ \"mode\": \"hashrate_split\","
        "  \"target_shares\": 100000, \"min_slice_s\": 5000, \"max_slice_s\": 0,"
        "  \"pools\": ["
        "    {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "    {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    dualpool_config_t c; char err[256];
    assert(config_parse_string(hi, &c, err, sizeof(err)) == 0);
    assert(c.target_shares == 1000);
    assert(c.min_slice_s == 3600);
    assert(c.max_slice_s == 3600);           /* raised to min after max clamped to 1 */
    assert(c.min_slice_s <= c.max_slice_s);

    /* under-range: zero/negative knobs floor to 1. */
    const char *lo =
        "{ \"mode\": \"hashrate_split\","
        "  \"target_shares\": 0, \"min_slice_s\": 0, \"max_slice_s\": -5,"
        "  \"pools\": ["
        "    {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "    {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    assert(config_parse_string(lo, &c, err, sizeof(err)) == 0);
    assert(c.target_shares == 1);
    assert(c.min_slice_s == 1);
    assert(c.max_slice_s == 1);

    /* an ordinary inverted pair (min>max, both in range) raises max up to min. */
    const char *inv =
        "{ \"mode\": \"hashrate_split\","
        "  \"target_shares\": 10, \"min_slice_s\": 90, \"max_slice_s\": 30,"
        "  \"pools\": ["
        "    {\"url\":\"a:1\",\"user\":\"u\",\"pass\":\"p\"},"
        "    {\"url\":\"b:2\",\"user\":\"u\",\"pass\":\"p\"} ] }";
    assert(config_parse_string(inv, &c, err, sizeof(err)) == 0);
    assert(c.min_slice_s == 90);
    assert(c.max_slice_s == 90);
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
    /* pool A carried explicit per-pool difficulty into the ckproxy config */
    assert(json_integer_value(json_object_get(root, "startdiff")) == 100000);
    assert(json_integer_value(json_object_get(root, "mindiff")) == 50000);
    json_decref(root);

    /* A pool with NO failover gets exactly ONE proxy entry (never duplicate the
     * primary — see ckproxy_config.c: duplicate entries corrupt shares on the
     * classic ckpool build). */
    char pathb[] = "/tmp/dualpool_ckproxyB.json";
    assert(ckproxy_config_write(&c.pools[1], 4002, "/tmp/sockB", pathb,
                                err, sizeof(err)) == 0);
    json_t *rb = json_load_file(pathb, 0, &je);
    assert(rb);
    json_t *pb = json_object_get(rb, "proxy");
    assert(json_is_array(pb) && json_array_size(pb) == 1);
    assert(strcmp(json_string_value(json_object_get(json_array_get(pb, 0), "url")), "poolB:3333") == 0);
    /* pool B set no difficulty => ckproxy config falls back to built-in defaults */
    assert(json_integer_value(json_object_get(rb, "startdiff")) == 42);
    assert(json_integer_value(json_object_get(rb, "mindiff")) == 1);
    json_decref(rb);
}

int main(void) {
    test_parse_full();
    test_defaults();
    test_hashrate_split();
    test_assume_extranonce();
    printf("config: parse full + defaults passed\n");
    test_x35_upgrade();
    test_x35_assume_extranonce_strict();
    printf("config: X.3.5 upgrade compat (fields intact, new keys default) passed\n");
    test_ratio_clamped();
    test_slice_knobs_clamped();
    test_requires_two_pools();
    printf("config: clamp + validation passed\n");
    test_ckproxy_emit();
    printf("config: ckproxy emit passed\n");
    printf("ALL config host tests passed\n");
    return 0;
}
