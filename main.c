/*
 * SPDX-FileCopyrightText: 2026 esp-claw contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/*
 * sb: read-only Supabase Management API through the device's "supabase_mgmt" API profile
 * (bearer = personal access token).
 *   sb_projects  projects with ref, region, status
 *   sb_health    service health of a project
 *   sb_usage     disk usage/config and API request counts (no billing quotas in the API)
 *   sb_branches  database branches
 *   sb_logs      latest log lines of one source (unified logs endpoint)
 * The device injects the token; the app never sees it. Outputs are compact column/row JSON.
 */
#define CLAW_ARENA_SIZE 1024
#include "claw.h"
#include "claw_util.h"

CLAW_DEFINE_ARENA()

#define PROFILE     "supabase_mgmt"
#define OUT_RESERVE 128 /* kept free while writing rows, for the closing text */

static int32_t s_t_projects = -1, s_t_health = -1, s_t_usage = -1, s_t_branches = -1, s_t_logs = -1;

static char s_resp[32 * 1024 + 1];
static uint32_t s_resp_len;
static char s_out[16 * 1024];
static claw_buf_t s_b;
static uint32_t s_mark;
static int s_trunc;
static char s_path[1024];
static claw_buf_t s_p;
static const char *s_args, *s_args_end;

/* ---------- small helpers (no libc) ---------- */

static int str_eq(const char *a, uint32_t an, const char *b)
{
    uint32_t i = 0;

    for (; i < an && b[i] && a[i] == b[i]; i++) {
    }
    return i == an && !b[i];
}

/* ---------- output ---------- */

/* Out of line: the SDK helpers are static inline and would otherwise be inlined at every call. */
#define NOINLINE __attribute__((noinline))

static NOINLINE void P(const char *s)
{
    cb_put(&s_b, s);
}

static NOINLINE void bput(claw_buf_t *b, const char *s)
{
    cb_put(b, s);
}

static NOINLINE void bput_u32(claw_buf_t *b, uint32_t v)
{
    cb_put_u32(b, v);
}

static void out_begin(void)
{
    s_b.buf = s_out;
    s_b.cap = sizeof(s_out) - OUT_RESERVE;
    s_b.len = 0;
    s_b.full = 0;
    s_out[0] = '\0';
    s_trunc = 0;
}

static void out_end(void)
{
    s_b.cap = sizeof(s_out);
    claw_tool_result(s_b.buf, s_b.len);
}

static void row_begin(void)
{
    s_mark = s_b.len;
}

/* Rolls back a row that did not fit; returns 0 then (stop adding rows). */
static int row_end(void)
{
    if (!s_b.full) {
        return 1;
    }
    s_b.len = s_mark;
    s_b.buf[s_mark] = '\0';
    s_b.full = 0;
    s_trunc = 1;
    return 0;
}

static NOINLINE void put_u64(uint64_t v)
{
    char tmp[20];
    char out[20];
    int n = 0;
    int i = 0;

    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        out[i++] = tmp[--n];
    }
    cb_putn(&s_b, out, (uint32_t)i);
}

static void put_i32(int32_t v)
{
    if (v < 0) {
        P("-");
        v = -v;
    }
    bput_u32(&s_b, (uint32_t)v);
}

/* Appends text from [p, e) as JSON string contents, at most `max` source bytes, cut at a character
 * boundary. Flags: SAFE_JSON = the text is already JSON-escaped (escape sequences are copied whole),
 * otherwise quotes/backslashes are escaped and control bytes/ANSI sequences dropped; SAFE_CUT = cut
 * silently (dates), else "..." marks a cut. Invalid UTF-8 bytes are dropped in all modes. */
#define SAFE_JSON 1
#define SAFE_CUT  2

static NOINLINE void put_safe(const char *p, const char *e, uint32_t max, int flags)
{
    uint32_t used = 0;
    int json = flags & SAFE_JSON;

    while (p < e) {
        unsigned char c = (unsigned char)*p;
        uint32_t unit = 1;

        if (json && c == '\\') {
            unit = p + 1 < e && p[1] == 'u' ? 6 : 2;
            if (unit == 6 && p + 12 <= e && (p[2] == 'd' || p[2] == 'D') &&
                    (p[3] == '8' || p[3] == '9' || p[3] == 'a' || p[3] == 'b' || p[3] == 'A' || p[3] == 'B')) {
                unit = 12; /* surrogate pair stays together */
            }
            if (p + unit > e) {
                break;
            }
        } else if (c >= 0x80) {
            unsigned char c1 = p + 1 < e ? (unsigned char)p[1] : 0;
            int ok = 1;

            unit = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            if (!unit || p + unit > e || (c == 0xe0 && c1 < 0xa0) || (c == 0xed && c1 > 0x9f) ||
                    (c == 0xf0 && c1 < 0x90) || (c == 0xf4 && c1 > 0x8f)) {
                ok = 0;
            }
            for (uint32_t i = 1; ok && i < unit; i++) {
                ok = ((unsigned char)p[i] & 0xc0) == 0x80;
            }
            if (!ok) {
                p++;
                continue;
            }
        } else if (!json && c == 0x1b) {
            p++;
            if (p < e && *p == '[') {
                for (p++; p < e && !(*p >= 0x40 && *p <= 0x7e); p++) {
                }
                p += p < e;
            }
            continue;
        } else if (!json && (c < 0x20 || c == 0x7f)) {
            if (c == '\t') {
                cb_putn(&s_b, " ", 1);
                used++;
            }
            p++;
            continue;
        }
        if (used + unit > max) {
            if (!(flags & SAFE_CUT)) {
                P("...");
            }
            return;
        }
        if (!json && (c == '"' || c == '\\')) {
            cb_putn(&s_b, "\\", 1);
        }
        cb_putn(&s_b, p, unit);
        used += unit;
        p += unit;
    }
}

/* The JSON string at v as a quoted string trimmed to `max` bytes; null for anything else. */
static NOINLINE void put_jstr(const char *v, const char *end, uint32_t max)
{
    const char *close;

    if (!v || v >= end || *v != '"' || !(close = cj_skip(v, end))) {
        P("null");
        return;
    }
    P("\"");
    put_safe(v + 1, close - 1, max, SAFE_JSON);
    P("\"");
}

/* The JSON string at v cut to `max` bytes without a marker (timestamps: 16 = minutes, 10 = date). */
static NOINLINE void put_jcut(const char *v, const char *end, uint32_t max)
{
    const char *close;

    if (!v || v >= end || *v != '"' || !(close = cj_skip(v, end))) {
        P("null");
        return;
    }
    P("\"");
    put_safe(v + 1, close - 1, max, SAFE_JSON | SAFE_CUT);
    P("\"");
}

/* A scalar (number, true/false/null) copied as-is; strings trimmed; objects/arrays become null. */
static NOINLINE void put_raw(const char *v, const char *end)
{
    const char *e;

    if (v && v < end && *v == '"') {
        put_jstr(v, end, 200);
    } else if (!v || v >= end || *v == '{' || *v == '[' || !(e = cj_skip(v, end)) || e - v > 24) {
        P("null");
    } else {
        cb_putn(&s_b, v, (uint32_t)(e - v));
    }
}

static const char *mem(const char *obj, const char *end, const char *k1, const char *k2)
{
    const char *v = cj_member(obj, end, k1);

    return k2 && v ? cj_member(v, end, k2) : v;
}

static NOINLINE void err_result(const char *msg, const char *detail)
{
    out_begin();
    P("{\"error\":\"");
    P(msg);
    if (detail) {
        P("\",\"value\":\"");
        put_safe(detail, detail + claw_strlen(detail), 200, 0);
    }
    P("\"}");
    out_end();
}

/* ---------- args ---------- */

static int32_t arg_str(const char *key, char *out, uint32_t cap)
{
    return cj_string(cj_member(s_args, s_args_end, key), s_args_end, out, cap);
}

static int64_t arg_int(const char *key, int64_t def)
{
    const char *v = cj_member(s_args, s_args_end, key);
    int64_t n;

    if (v && *v == '"') {
        v++;
    }
    n = v ? cj_uint(v, s_args_end) : -1;
    return n < 0 ? def : n;
}

/* ---------- HTTP ---------- */

static void path_begin(const char *s)
{
    s_p.buf = s_path;
    s_p.cap = sizeof(s_path);
    s_p.len = 0;
    s_p.full = 0;
    bput(&s_p, s);
}

/* Percent-encodes everything but unreserved characters (query values). */
static NOINLINE void path_enc(const char *s)
{
    static const char hex[] = "0123456789ABCDEF";

    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;

        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                c == '.' || c == '~') {
            cb_putn(&s_p, s, 1);
        } else {
            char e[3] = { '%', hex[c >> 4], hex[c & 15] };

            cb_putn(&s_p, e, 3);
        }
    }
}

static NOINLINE int32_t get(void)
{
    int32_t rc;
    int32_t n;

    if (s_p.full) {
        s_resp_len = 0;
        return -CLAW_E2BIG;
    }
    rc = claw_http_profile_request(CLAW_S(PROFILE), CLAW_S("GET"), s_p.buf, s_p.len, "", 0, "", 0);
    s_resp_len = 0;
    while (s_resp_len < sizeof(s_resp) - 1 &&
            (n = claw_http_response_read(s_resp + s_resp_len, sizeof(s_resp) - 1 - s_resp_len)) > 0) {
        s_resp_len += (uint32_t)n;
    }
    s_resp[s_resp_len] = '\0';
    return rc;
}

/* Appends the API's error ("message", or "error" as string or {message}) or the device's text. */
static void put_api_error(int32_t rc)
{
    const char *end = s_resp + s_resp_len;
    const char *m = cj_member(s_resp, end, "message");
    const char *e = cj_member(s_resp, end, "error");

    P("\"status\":");
    put_i32(rc);
    if (!m && e && *e == '{') {
        m = cj_member(e, end, "message");
    } else if (!m && e && *e == '"') {
        m = e;
    }
    if (m) {
        P(",\"message\":");
        put_jstr(m, end, 300);
    } else if (s_resp_len) {
        P(",\"detail\":\"");
        put_safe(s_resp, end, 300, 0);
        P("\"");
    }
    if (rc == 401) {
        P(",\"hint\":\"personal access token invalid or expired\"");
    } else if (rc == 403 || rc == 404) {
        P(",\"hint\":\"no access to this project, unknown ref, or feature not on this plan\"");
    }
}

static NOINLINE void fail(const char *what, int32_t rc)
{
    out_begin();
    P("{\"error\":\"");
    P(what);
    P(" failed\",");
    put_api_error(rc);
    P("}");
    out_end();
}

/* ---------- helpers ---------- */

static char s_ref[24];

/* Project ref: 20 lowercase letters. 1 = ok, else result set. */
static int ref_arg(void)
{
    int32_t n = arg_str("ref", s_ref, sizeof(s_ref));
    int ok = n == 20;

    for (int32_t i = 0; ok && i < n; i++) {
        ok = s_ref[i] >= 'a' && s_ref[i] <= 'z';
    }
    if (!ok) {
        err_result("ref must be the 20-letter project ref (see sb_projects)", n > 0 ? s_ref : 0);
    }
    return ok;
}

static void project_path(const char *suffix)
{
    path_begin("/v1/projects/");
    bput(&s_p, s_ref);
    bput(&s_p, suffix);
}

/* "YYYY-MM-DDTHH:MM:SSZ" for a Unix time in seconds. */
static NOINLINE void put_iso(claw_buf_t *b, int64_t secs)
{
    int64_t days = secs / 86400;
    int64_t sod = secs % 86400;
    /* civil_from_days (H. Hinnant) */
    int64_t z = days + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    int64_t f[6];
    char s[21];

    y += m <= 2;
    f[0] = y / 100;
    f[1] = y % 100;
    f[2] = m;
    f[3] = d;
    f[4] = sod / 3600;
    f[5] = sod / 60 % 60;
    for (int i = 0; i < 6; i++) {
        static const uint8_t pos[] = { 0, 2, 5, 8, 11, 14 };

        s[pos[i]] = (char)('0' + f[i] / 10);
        s[pos[i] + 1] = (char)('0' + f[i] % 10);
    }
    s[4] = '-';
    s[7] = '-';
    s[10] = 'T';
    s[13] = ':';
    s[16] = ':';
    s[17] = (char)('0' + sod % 60 / 10);
    s[18] = (char)('0' + sod % 60 % 10);
    s[19] = 'Z';
    s[20] = '\0';
    bput(b, s);
}

typedef int (*row_fn)(const char *item, const char *end);

/* Appends a row for every complete element of `arr`; returns the rows added (s_trunc on overflow). */
static NOINLINE uint32_t rows(const char *arr, const char *end, row_fn fn)
{
    uint32_t n = 0;

    for (const char *it = cj_first(arr, end); it; it = cj_next(it, end)) {
        if (!cj_skip(it, end)) {
            s_trunc = 1;
            break;
        }
        row_begin();
        P(n ? ",[" : "[");
        fn(it, end);
        P("]");
        if (!row_end()) {
            break;
        }
        n++;
    }
    return n;
}

static void close_rows(uint32_t count)
{
    s_b.cap = sizeof(s_out);
    P("],\"count\":");
    bput_u32(&s_b, count);
    P(s_trunc ? ",\"truncated\":true}" : "}");
    out_end();
}

/* ---------- row writers ---------- */

static int row_project(const char *it, const char *end)
{
    const char *ref = cj_member(it, end, "ref");

    put_jstr(cj_member(it, end, "name"), end, 80);
    P(",");
    put_jstr(ref ? ref : cj_member(it, end, "id"), end, 24);
    P(",");
    put_jstr(cj_member(it, end, "region"), end, 24);
    P(",");
    put_jstr(cj_member(it, end, "status"), end, 24);
    P(",");
    put_jstr(mem(it, end, "database", "version"), end, 24);
    return 1;
}

static int row_health(const char *it, const char *end)
{
    put_jstr(cj_member(it, end, "name"), end, 20);
    P(",");
    put_jstr(cj_member(it, end, "status"), end, 20);
    P(",");
    put_jstr(cj_member(it, end, "error"), end, 200);
    return 1;
}

static int row_branch(const char *it, const char *end)
{
    put_jstr(cj_member(it, end, "name"), end, 60);
    P(",");
    put_jstr(cj_member(it, end, "git_branch"), end, 60);
    P(",");
    put_raw(cj_member(it, end, "is_default"), end);
    P(",");
    put_raw(cj_member(it, end, "persistent"), end);
    P(",");
    put_jstr(cj_member(it, end, "status"), end, 24);
    P(",");
    put_jstr(cj_member(it, end, "project_ref"), end, 24);
    P(",");
    put_jcut(cj_member(it, end, "updated_at"), end, 16);
    return 1;
}

/* Log timestamp: microseconds (or ms/s) since the epoch, or a date string (format UNVERIFIED). */
static int row_log(const char *it, const char *end)
{
    const char *t = cj_member(it, end, "timestamp");
    int64_t ts = cj_uint(t, end);

    if (t && *t == '"') {
        put_jcut(t, end, 19);
    } else if (ts > 0) {
        P("\"");
        put_iso(&s_b, ts > 100000000000000LL ? ts / 1000000 : ts > 100000000000LL ? ts / 1000 : ts);
        P("\"");
    } else {
        P("null");
    }
    P(",");
    put_jstr(cj_member(it, end, "event_message"), end, 300);
    return 1;
}

/* ---------- tools ---------- */

static void tool_projects(void)
{
    int32_t rc;

    path_begin("/v1/projects");
    rc = get();
    if (rc != 200) {
        fail("GET /v1/projects", rc);
        return;
    }
    out_begin();
    P("{\"cols\":[\"name\",\"ref\",\"region\",\"status\",\"postgres\"],\"rows\":[");
    close_rows(rows(s_resp, s_resp + s_resp_len, row_project));
}

static void tool_health(void)
{
    int32_t rc;

    if (!ref_arg()) {
        return;
    }
    project_path("/health?services=auth,db,realtime,rest,storage");
    rc = get();
    if (rc != 200) {
        fail("GET health", rc);
        return;
    }
    out_begin();
    P("{\"ref\":\"");
    P(s_ref);
    P("\",\"cols\":[\"service\",\"status\",\"error\"],\"rows\":[");
    close_rows(rows(s_resp, s_resp + s_resp_len, row_health));
}

/* Disk and API request usage. The Management API has no plan-quota/billing usage endpoint;
 * database size would need SQL (POST /database/query, which the device would ask to confirm). */
static void tool_usage(void)
{
    static const char *const keys[] = { "total_rest_requests", "total_auth_requests", "total_storage_requests",
                                        "total_realtime_requests"
                                      };
    static const char *const outk[] = { "rest", "auth", "storage", "realtime" };
    const char *end;
    const char *err;
    int32_t rc;

    if (!ref_arg()) {
        return;
    }
    out_begin();
    P("{\"ref\":\"");
    P(s_ref);
    P("\",\"disk\":{");
    project_path("/config/disk/util");
    rc = get();
    end = s_resp + s_resp_len;
    if (rc == 200) {
        const char *m = cj_member(s_resp, end, "metrics");

        P("\"used_bytes\":");
        put_raw(cj_member(m, end, "fs_used_bytes"), end);
        P(",\"size_bytes\":");
        put_raw(cj_member(m, end, "fs_size_bytes"), end);
        P(",\"avail_bytes\":");
        put_raw(cj_member(m, end, "fs_avail_bytes"), end);
        P(",\"at\":");
        put_jcut(cj_member(s_resp, end, "timestamp"), end, 16);
    } else {
        put_api_error(rc);
    }
    P("},\"disk_config\":{");
    project_path("/config/disk");
    rc = get();
    end = s_resp + s_resp_len;
    if (rc == 200) {
        const char *a = cj_member(s_resp, end, "attributes");

        P("\"size_gb\":");
        put_raw(cj_member(a, end, "size_gb"), end);
        P(",\"type\":");
        put_jstr(cj_member(a, end, "type"), end, 8);
        P(",\"iops\":");
        put_raw(cj_member(a, end, "iops"), end);
    } else {
        put_api_error(rc);
    }
    P("},\"api_requests_24h\":{");
    project_path("/analytics/endpoints/usage.api-counts?interval=1day");
    rc = get();
    end = s_resp + s_resp_len;
    err = cj_member(s_resp, end, "error");
    if (rc == 200 && (!err || *err == 'n')) {
        uint64_t sums[4] = { 0, 0, 0, 0 };

        for (const char *it = cj_first(cj_member(s_resp, end, "result"), end); it; it = cj_next(it, end)) {
            for (int i = 0; i < 4; i++) {
                int64_t v = cj_uint(cj_member(it, end, keys[i]), end);

                sums[i] += v > 0 ? (uint64_t)v : 0;
            }
        }
        for (int i = 0; i < 4; i++) {
            P(i ? ",\"" : "\"");
            P(outk[i]);
            P("\":");
            put_u64(sums[i]);
        }
    } else {
        put_api_error(rc);
    }
    P("}}");
    out_end();
}

static void tool_branches(void)
{
    int32_t rc;

    if (!ref_arg()) {
        return;
    }
    project_path("/branches");
    rc = get();
    if (rc != 200) {
        fail("GET branches", rc);
        return;
    }
    out_begin();
    P("{\"ref\":\"");
    P(s_ref);
    P("\",\"cols\":[\"name\",\"git_branch\",\"default\",\"persistent\",\"status\",\"branch_ref\",\"updated\"],\"rows\":[");
    close_rows(rows(s_resp, s_resp + s_resp_len, row_branch));
}

/* Log sources of the unified "logs" table (source_name). Names follow the Supabase log explorer;
 * the list for the new ClickHouse-based endpoint is UNVERIFIED. */
static const char *const s_sources[][2] = {
    { "api", "edge_logs" }, { "edge", "edge_logs" }, { "postgres", "postgres_logs" }, { "db", "postgres_logs" },
    { "auth", "auth_logs" }, { "functions", "function_logs" }, { "functions_edge", "function_edge_logs" },
    { "realtime", "realtime_logs" }, { "storage", "storage_logs" }, { "rest", "postgrest_logs" },
    { "postgrest", "postgrest_logs" }, { "pooler", "supavisor_logs" },
};

static void tool_logs(void)
{
    char src[24];
    char contains[64];
    int32_t sn = arg_str("source", src, sizeof(src));
    int32_t cn = arg_str("contains", contains, sizeof(contains));
    int64_t minutes = arg_int("minutes", 60);
    int64_t limit = arg_int("limit", 25);
    int64_t now = claw_now_ms() / 1000;
    const char *table = 0;
    char sql[320];
    char ts[24];
    claw_buf_t q = { sql, sizeof(sql), 0, 0 };
    claw_buf_t t = { ts, sizeof(ts), 0, 0 };
    const char *end;
    const char *err;
    int32_t rc;

    if (!ref_arg()) {
        return;
    }
    for (uint32_t i = 0; sn > 0 && i < sizeof(s_sources) / sizeof(s_sources[0]) && !table; i++) {
        if (str_eq(src, (uint32_t)sn, s_sources[i][0]) || str_eq(src, (uint32_t)sn, s_sources[i][1])) {
            table = s_sources[i][1];
        }
    }
    if (!table) {
        err_result("source must be api, postgres, auth, functions, functions_edge, realtime, storage, rest or pooler",
                   sn > 0 ? src : 0);
        return;
    }
    if (minutes < 1 || minutes > 1440 || limit < 1 || limit > 50) {
        err_result("minutes must be 1-1440 and limit 1-50", 0);
        return;
    }
    for (int32_t i = 0; i < cn; i++) { /* goes into a SQL string literal: plain characters only */
        char c = contains[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '_' ||
                c == '-' || c == '.' || c == ':' || c == '/')) {
            err_result("contains may only use letters, digits, space and _-.:/", 0);
            return;
        }
    }
    if (now < 1577836800) {
        err_result("device clock not synced yet", 0);
        return;
    }
    bput(&q, "select timestamp, event_message from logs where source_name = '");
    bput(&q, table);
    bput(&q, "'");
    if (cn > 0) {
        bput(&q, " and positionCaseInsensitive(event_message, '");
        bput(&q, contains);
        bput(&q, "') > 0");
    }
    bput(&q, " order by timestamp desc limit ");
    bput_u32(&q, (uint32_t)limit);
    project_path("/analytics/endpoints/logs?sql=");
    path_enc(sql);
    bput(&s_p, "&iso_timestamp_start=");
    put_iso(&t, now - minutes * 60);
    path_enc(ts);
    bput(&s_p, "&iso_timestamp_end=");
    t.len = 0;
    put_iso(&t, now);
    path_enc(ts);
    rc = get();
    end = s_resp + s_resp_len;
    err = cj_member(s_resp, end, "error");
    if (rc != 200 || (err && *err != 'n')) {
        fail("GET analytics logs", rc);
        return;
    }
    out_begin();
    P("{\"ref\":\"");
    P(s_ref);
    P("\",\"source\":\"");
    P(table);
    P("\",\"minutes\":");
    bput_u32(&s_b, (uint32_t)minutes);
    P(",\"cols\":[\"time\",\"message\"],\"rows\":[");
    close_rows(rows(cj_member(s_resp, end, "result"), end, row_log));
}

/* ---------- registration and dispatch ---------- */

#define REF_PROP "\"ref\":{\"type\":\"string\",\"description\":\"20-letter project ref from sb_projects\"}"

CLAW_EXPORT("app_init") void app_init(void)
{
    s_t_projects = CLAW_REGISTER("projects", "List Supabase projects: name, ref, region, status, Postgres version.",
                                 "{\"type\":\"object\",\"properties\":{}}");
    s_t_health = CLAW_REGISTER("health", "Health of a project's services (auth, db, realtime, rest, storage).",
                               "{\"type\":\"object\",\"properties\":{" REF_PROP "},\"required\":[\"ref\"]}");
    s_t_usage = CLAW_REGISTER("usage", "Disk usage (used/size/available bytes), disk config and API request counts "
                              "of the last 24 h of a project. No billing quotas (not in the Management API).",
                              "{\"type\":\"object\",\"properties\":{" REF_PROP "},\"required\":[\"ref\"]}");
    s_t_branches = CLAW_REGISTER("branches", "Database branches of a project (needs branching enabled).",
                                 "{\"type\":\"object\",\"properties\":{" REF_PROP "},\"required\":[\"ref\"]}");
    s_t_logs = CLAW_REGISTER("logs", "Latest log lines of a project: source api|postgres|auth|functions|"
                             "functions_edge|realtime|storage|rest|pooler, last `minutes` (1-1440, default 60), "
                             "optional `contains` text filter, limit 1-50 (25).",
                             "{\"type\":\"object\",\"properties\":{" REF_PROP ",\"source\":{\"type\":\"string\"},"
                             "\"minutes\":{\"type\":\"integer\"},\"contains\":{\"type\":\"string\"},"
                             "\"limit\":{\"type\":\"integer\"}},\"required\":[\"ref\",\"source\"]}");
}

CLAW_EXPORT("tool_call") int32_t tool_call(int32_t tool, const char *args, uint32_t len)
{
    s_args = args;
    s_args_end = args + len;
    if (tool == s_t_projects) {
        tool_projects();
    } else if (tool == s_t_health) {
        tool_health();
    } else if (tool == s_t_usage) {
        tool_usage();
    } else if (tool == s_t_branches) {
        tool_branches();
    } else if (tool == s_t_logs) {
        tool_logs();
    } else {
        return -1;
    }
    return 0;
}
