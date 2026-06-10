/*
 * cuvs_objstore.c — GCS object storage client for pg_cuvs Phase 3C.
 *
 * Daemon only (not linked into the PostgreSQL .so extension).
 * Requires: -lcurl -lssl -lcrypto
 *
 * Design notes:
 *   - Authentication: instance metadata server first; service account JSON fallback.
 *   - Token cached process-globally; refreshed 5 min before expiry.
 *   - Upload is best-effort and runs in a detached thread — upload failure never
 *     fails a CREATE INDEX (local artifact is already durable).
 *   - Manifest-last upload: artifacts uploaded before manifest.json so a missing
 *     manifest always signals an incomplete upload.
 *   - Download verifies SHA256 of each file before accepting it as the local cache.
 *   - Heap compatibility is a hard reject: relfilenode mismatch → artifact not loaded.
 */

#include "cuvs_objstore.h"
#include "cuvs_util.h"
#include "cuvs_ipc.h"   /* CUVS_METRIC_* */
#include "cuvs_version.h"   /* PG_CUVS_VERSION, CUVS_BUILD_VERSION (ADR-013 manifest stamps) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/bio.h>

/* ----------------------------------------------------------------
 * Internal types
 * ---------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} RespBuf;

/* ----------------------------------------------------------------
 * Token cache (process-global, mutex-protected)
 * ---------------------------------------------------------------- */

static char            g_token[2048]   = "";
static time_t          g_token_expiry  = 0;
static pthread_mutex_t g_token_mutex   = PTHREAD_MUTEX_INITIALIZER;

/* ----------------------------------------------------------------
 * Utility: parse "gs://bucket[/prefix]" into bucket + object prefix
 * ---------------------------------------------------------------- */

static int
parse_gcs_uri(const char *uri, char *bucket, size_t bsz,
              char *prefix, size_t psz)
{
    if (strncmp(uri, "gs://", 5) != 0)
        return -1;

    const char *rest  = uri + 5;
    const char *slash = strchr(rest, '/');

    if (!slash) {
        size_t blen = strlen(rest);
        if (blen >= bsz) return -1;
        memcpy(bucket, rest, blen + 1);
        prefix[0] = '\0';
    } else {
        size_t blen = (size_t)(slash - rest);
        if (blen >= bsz) return -1;
        memcpy(bucket, rest, blen);
        bucket[blen] = '\0';

        const char *p    = slash + 1;
        size_t      plen = strlen(p);
        while (plen > 0 && p[plen - 1] == '/')
            plen--;
        if (plen >= psz) return -1;
        memcpy(prefix, p, plen);
        prefix[plen] = '\0';
    }
    return 0;
}

/* ----------------------------------------------------------------
 * URL percent-encoding (encodes '/' as %2F — for query/path params)
 * ---------------------------------------------------------------- */

static void
url_encode(const char *in, char *out, size_t out_size)
{
    static const char safe[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    size_t j = 0;
    for (const char *p = in; *p && j + 4 < out_size; p++) {
        if (strchr(safe, (unsigned char)*p)) {
            out[j++] = *p;
        } else {
            snprintf(out + j, out_size - j, "%%%02X", (unsigned char)*p);
            j += 3;
        }
    }
    out[j] = '\0';
}

/* ----------------------------------------------------------------
 * Base64url encoding (no padding; + → -; / → _)
 * ---------------------------------------------------------------- */

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void
base64url_encode(const unsigned char *in, size_t in_len,
                 char *out, size_t *out_len)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= (uint32_t)in[i + 2];
        out[j++] = B64URL[(v >> 18) & 63];
        out[j++] = B64URL[(v >> 12) & 63];
        /* Replace padding '=' with NUL so we can strip them below */
        out[j++] = (i + 1 < in_len) ? B64URL[(v >> 6) & 63] : '\0';
        out[j++] = (i + 2 < in_len) ? B64URL[v & 63]        : '\0';
    }
    /* Compact: remove the embedded NULs that replaced padding */
    size_t k = 0;
    for (size_t i = 0; i < j; i++)
        if (out[i] != '\0')
            out[k++] = out[i];
    out[k] = '\0';
    if (out_len) *out_len = k;
}

/* ----------------------------------------------------------------
 * libcurl callbacks
 * ---------------------------------------------------------------- */

static size_t
write_to_buf(void *ptr, size_t sz, size_t nmemb, void *userp)
{
    RespBuf *rb  = userp;
    size_t   add = sz * nmemb;
    if (rb->size + add + 1 > rb->cap) {
        size_t  ncap = rb->cap + add + 4096;
        char   *nd   = realloc(rb->data, ncap);
        if (!nd) return 0;
        rb->data = nd;
        rb->cap  = ncap;
    }
    memcpy(rb->data + rb->size, ptr, add);
    rb->size           += add;
    rb->data[rb->size]  = '\0';
    return add;
}

static size_t
read_from_file(void *ptr, size_t sz, size_t nmemb, void *userp)
{
    return fread(ptr, sz, nmemb, (FILE *)userp);
}

/* ----------------------------------------------------------------
 * Minimal JSON field extraction (for our own well-structured JSON)
 * ---------------------------------------------------------------- */

/* Extract a string value: "key":"value" */
static int
json_get_str(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;  /* tolerate pretty-print ws */
    if (*p != '"') return -1;
    p++;                                  /* opening quote */
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < out_sz)
        out[i++] = *p++;
    out[i] = '\0';
    return (*p == '"') ? 0 : -1;
}

/* Extract an unsigned 32-bit integer value: "key":12345 */
static int
json_get_u32(const char *json, const char *key, uint32_t *out)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    char *end;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* Extract a signed 64-bit integer value: "key":12345 */
static int
json_get_i64(const char *json, const char *key, int64_t *out)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    while (*p == ' ') p++;
    char *end;
    long long v = strtoll(p, &end, 10);
    if (end == p) return -1;
    *out = (int64_t)v;
    return 0;
}

/* Locate a JSON object value: returns pointer to content after '{' */
static const char *
json_find_obj(const char *json, const char *key)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;  /* tolerate pretty-print ws */
    return (*p == '{') ? p + 1 : NULL;
}

/* ----------------------------------------------------------------
 * GCS token: instance metadata
 * ---------------------------------------------------------------- */

static int
fetch_metadata_token(char *tok, size_t tok_sz, long *expires_in_out)
{
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, "Metadata-Flavor: Google");

    RespBuf rb = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_URL,
        "http://metadata.google.internal/computeMetadata/v1"
        "/instance/service-accounts/default/token");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,   hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       5L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,   1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    int ret = -1;
    if (rc == CURLE_OK && rb.data) {
        if (json_get_str(rb.data, "access_token", tok, tok_sz) == 0) {
            int64_t ei = 3600;
            json_get_i64(rb.data, "expires_in", &ei);
            if (expires_in_out) *expires_in_out = (long)ei;
            ret = 0;
        }
    }
    free(rb.data);
    return ret;
}

/* ----------------------------------------------------------------
 * GCS token: service account JSON → JWT exchange
 * ---------------------------------------------------------------- */

/* Extract a JSON string that may contain \n escapes (e.g. PEM private key). */
static int
sa_json_extract(const char *json, const char *key, char *out, size_t out_sz)
{
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) return -1;
    p += strlen(pat);
    size_t i = 0;
    while (*p && i + 2 < out_sz) {
        if (p[0] == '\\' && p[1] == 'n')  { out[i++] = '\n'; p += 2; }
        else if (p[0] == '\\' && p[1] == '"') { out[i++] = '"';  p += 2; }
        else if (p[0] == '\\' && p[1] == '\\') { out[i++] = '\\'; p += 2; }
        else if (*p == '"') break;
        else out[i++] = *p++;
    }
    out[i] = '\0';
    return 0;
}

static int
fetch_jwt_token(const char *key_file, char *tok, size_t tok_sz,
                long *expires_in_out)
{
    FILE *f = fopen(key_file, "r");
    if (!f) {
        LOG_WARN("[objstore] gcs_key_file not found: %s\n", key_file);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    rewind(f);
    if (flen <= 0 || flen > 65536) { fclose(f); return -1; }

    char *sa_json = malloc((size_t)flen + 1);
    if (!sa_json) { fclose(f); return -1; }
    fread(sa_json, 1, (size_t)flen, f);
    sa_json[flen] = '\0';
    fclose(f);

    char email[256] = "", pem[8192] = "";
    sa_json_extract(sa_json, "client_email", email, sizeof(email));
    sa_json_extract(sa_json, "private_key",  pem,   sizeof(pem));
    free(sa_json);

    if (!email[0] || !pem[0]) {
        LOG_WARN("[objstore] SA JSON missing client_email or private_key\n");
        return -1;
    }

    /* Build JWT header + claims */
    time_t now = time(NULL);
    const char header_json[] = "{\"alg\":\"RS256\",\"typ\":\"JWT\"}";
    char   claims_json[512];
    snprintf(claims_json, sizeof(claims_json),
             "{\"iss\":\"%s\","
             "\"scope\":\"https://www.googleapis.com/auth/devstorage.read_write\","
             "\"aud\":\"https://oauth2.googleapis.com/token\","
             "\"iat\":%lld,\"exp\":%lld}",
             email, (long long)now, (long long)(now + 3600));

    char b64hdr[256], b64clm[1024];
    size_t b64hdr_len, b64clm_len;
    base64url_encode((const unsigned char *)header_json, strlen(header_json),
                     b64hdr, &b64hdr_len);
    base64url_encode((const unsigned char *)claims_json, strlen(claims_json),
                     b64clm, &b64clm_len);

    char signing_input[2048];
    int si_len = snprintf(signing_input, sizeof(signing_input),
                          "%s.%s", b64hdr, b64clm);
    if (si_len <= 0 || (size_t)si_len >= sizeof(signing_input)) return -1;

    /* Load RSA private key from PEM */
    BIO *bio = BIO_new_mem_buf(pem, (int)strlen(pem));
    EVP_PKEY *pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
    BIO_free(bio);
    if (!pkey) {
        LOG_WARN("[objstore] failed to parse private_key from SA JSON\n");
        return -1;
    }

    /* Sign with RSA-SHA256 */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (EVP_DigestSignInit(ctx, NULL, EVP_sha256(), NULL, pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return -1;
    }
    EVP_DigestSignUpdate(ctx, signing_input, (size_t)si_len);
    size_t sig_len = 0;
    EVP_DigestSignFinal(ctx, NULL, &sig_len);
    unsigned char *sig = malloc(sig_len);
    if (!sig) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return -1;
    }
    EVP_DigestSignFinal(ctx, sig, &sig_len);
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    char b64sig[1024];
    size_t b64sig_len;
    base64url_encode(sig, sig_len, b64sig, &b64sig_len);
    free(sig);

    /* Full JWT: header.claims.signature */
    char jwt[4096];
    snprintf(jwt, sizeof(jwt), "%s.%s", signing_input, b64sig);

    /* Exchange JWT for access token */
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    /* URL-encode the grant_type; assertion is already base64url (safe chars) */
    char post_data[4200];
    snprintf(post_data, sizeof(post_data),
             "grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type"
             "%%3Ajwt-bearer&assertion=%s", jwt);

    RespBuf rb = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_URL,           "https://oauth2.googleapis.com/token");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    post_data);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       15L);

    CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    int ret = -1;
    if (rc == CURLE_OK && rb.data) {
        if (json_get_str(rb.data, "access_token", tok, tok_sz) == 0) {
            int64_t ei = 3600;
            json_get_i64(rb.data, "expires_in", &ei);
            if (expires_in_out) *expires_in_out = (long)ei;
            ret = 0;
        } else {
            LOG_WARN("[objstore] JWT token exchange failed: %.200s\n", rb.data);
        }
    }
    free(rb.data);
    return ret;
}

/* Get (or refresh) a GCS bearer token. Thread-safe.
 * Tries instance metadata first; falls back to service account JWT. */
static int
get_gcs_token(const char *key_file, char *tok_out, size_t tok_sz)
{
    pthread_mutex_lock(&g_token_mutex);

    time_t now = time(NULL);
    /* Use cached token if it still has > 5 min of life */
    if (g_token[0] != '\0' && now < g_token_expiry - 300) {
        size_t tlen = strlen(g_token);
        if (tlen < tok_sz) {
            memcpy(tok_out, g_token, tlen + 1);
            pthread_mutex_unlock(&g_token_mutex);
            return 0;
        }
    }

    long expires_in = 3600;
    int  rc = fetch_metadata_token(g_token, sizeof(g_token), &expires_in);
    if (rc != 0 && key_file && key_file[0] != '\0')
        rc = fetch_jwt_token(key_file, g_token, sizeof(g_token), &expires_in);

    if (rc == 0) {
        g_token_expiry = now + expires_in;
        size_t tlen = strlen(g_token);
        if (tlen < tok_sz)
            memcpy(tok_out, g_token, tlen + 1);
        else
            rc = -1;
    } else {
        LOG_WARN("[objstore] failed to obtain GCS bearer token "
                 "(no instance metadata? set cuvs.gcs_key_file for SA auth)\n");
        g_token[0]    = '\0';
        g_token_expiry = 0;
    }

    pthread_mutex_unlock(&g_token_mutex);
    return rc;
}

/* ----------------------------------------------------------------
 * SHA256 of a local file (hex-encoded)
 * ---------------------------------------------------------------- */

int
cuvs_sha256_file(const char *path, char hex_out[65])
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) { fclose(f); return -1; }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
        EVP_MD_CTX_free(ctx); fclose(f); return -1;
    }

    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        EVP_DigestUpdate(ctx, buf, n);
    fclose(f);

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int  hlen = 0;
    if (EVP_DigestFinal_ex(ctx, hash, &hlen) != 1) {
        EVP_MD_CTX_free(ctx); return -1;
    }
    EVP_MD_CTX_free(ctx);

    for (unsigned int i = 0; i < hlen; i++)
        snprintf(hex_out + 2 * i, 3, "%02x", hash[i]);
    hex_out[hlen * 2] = '\0';
    return 0;
}

/* ----------------------------------------------------------------
 * GCS REST operations (libcurl)
 * ---------------------------------------------------------------- */

/* Upload a local file to gs://bucket/object_name */
static int
gcs_upload_file(const char *bucket, const char *object_name,
                const char *local_path, const char *token)
{
    struct stat st;
    if (stat(local_path, &st) != 0) return -1;

    FILE *f = fopen(local_path, "rb");
    if (!f) return -1;

    char enc_name[2048];
    url_encode(object_name, enc_name, sizeof(enc_name));

    char url[2560];
    snprintf(url, sizeof(url),
             "https://storage.googleapis.com/upload/storage/v1/b/%s/o"
             "?uploadType=media&name=%s", bucket, enc_name);

    char auth_hdr[2560];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");

    CURL *curl = curl_easy_init();
    if (!curl) { fclose(f); curl_slist_free_all(hdrs); return -1; }

    RespBuf rb = {NULL, 0, 0};
    /* The GCS JSON media-upload endpoint (/upload/.../o?uploadType=media)
     * requires POST; a PUT (CURLOPT_UPLOAD) returns HTTP 404. Stream the file as
     * the POST body via the read callback with a known length. */
    curl_easy_setopt(curl, CURLOPT_URL,                 url);
    curl_easy_setopt(curl, CURLOPT_POST,                1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)st.st_size);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,          hdrs);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION,        read_from_file);
    curl_easy_setopt(curl, CURLOPT_READDATA,            f);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,       write_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,           &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,             7200L); /* 2 h for large files */
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,         1L);

    CURLcode rc = curl_easy_perform(curl);
    long     http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    fclose(f);
    free(rb.data);

    if (rc != CURLE_OK) {
        LOG_WARN("[objstore] upload FAILED: %s (HTTP %ld) object=%s\n",
                 curl_easy_strerror(rc), http_code, object_name);
        return -1;
    }
    return 0;
}

/* Upload a string as a GCS object (for small JSON manifests). */
static int
gcs_upload_string(const char *bucket, const char *object_name,
                  const char *content, const char *token)
{
    char enc_name[2048];
    url_encode(object_name, enc_name, sizeof(enc_name));

    char url[2560];
    snprintf(url, sizeof(url),
             "https://storage.googleapis.com/upload/storage/v1/b/%s/o"
             "?uploadType=media&name=%s", bucket, enc_name);

    char auth_hdr[2560];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    CURL *curl = curl_easy_init();
    if (!curl) { curl_slist_free_all(hdrs); return -1; }

    RespBuf rb = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    content);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(content));
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,   1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    free(rb.data);

    return (rc == CURLE_OK) ? 0 : -1;
}

/* Download a GCS object to a local file.
 * Writes to a .tmp path first; renames on success. */
static int
gcs_download_file(const char *bucket, const char *object_name,
                  const char *local_path, const char *token)
{
    char enc_name[2048];
    url_encode(object_name, enc_name, sizeof(enc_name));

    char url[2560];
    snprintf(url, sizeof(url),
             "https://storage.googleapis.com/download/storage/v1/b/%s/o/%s?alt=media",
             bucket, enc_name);

    char auth_hdr[2560];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    char tmp_path[1024];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", local_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) return -1;

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(f); unlink(tmp_path);
        curl_slist_free_all(hdrs);
        return -1;
    }

    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     f);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       7200L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,   1L);

    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    fclose(f);

    if (rc != CURLE_OK) {
        LOG_WARN("[objstore] download FAILED: %s (HTTP %ld) object=%s\n",
                 curl_easy_strerror(rc), http_code, object_name);
        unlink(tmp_path);
        return -1;
    }

    if (rename(tmp_path, local_path) != 0) {
        LOG_WARN("[objstore] rename %s -> %s FAILED errno=%d\n",
                 tmp_path, local_path, errno);
        unlink(tmp_path);
        return -1;
    }
    return 0;
}

/* Fetch a GCS object as a heap-allocated string. Caller must free(). */
static char *
gcs_fetch_string(const char *bucket, const char *object_name, const char *token)
{
    char enc_name[2048];
    url_encode(object_name, enc_name, sizeof(enc_name));

    char url[2560];
    snprintf(url, sizeof(url),
             "https://storage.googleapis.com/download/storage/v1/b/%s/o/%s?alt=media",
             bucket, enc_name);

    char auth_hdr[2560];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", token);

    struct curl_slist *hdrs = NULL;
    hdrs = curl_slist_append(hdrs, auth_hdr);

    CURL *curl = curl_easy_init();
    if (!curl) { curl_slist_free_all(hdrs); return NULL; }

    RespBuf rb = {NULL, 0, 0};
    curl_easy_setopt(curl, CURLOPT_URL,           url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_buf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,     &rb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,       30L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR,   1L);

    CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        free(rb.data);
        return NULL;
    }
    return rb.data;  /* caller frees */
}

/* ----------------------------------------------------------------
 * Manifest JSON serialization / deserialization
 * ---------------------------------------------------------------- */

static void
manifest_to_json(const CuvsManifest *m, char *out, size_t out_sz)
{
    if (m->shard_count >= 2) {
        snprintf(out, out_sz,
            "{\n"
            "  \"pg_cuvs_version\": \"%s\",\n"
            "  \"cuvs_version\": \"%s\",\n"
            "  \"database_oid\": %u,\n"
            "  \"table_oid\": %u,\n"
            "  \"index_oid\": %u,\n"
            "  \"relfilenode\": %u,\n"
            "  \"base_generation\": %u,\n"
            "  \"metric\": \"%s\",\n"
            "  \"dim\": %u,\n"
            "  \"vector_count\": %lld,\n"
            "  \"build_timestamp\": %lld,\n"
            "  \"shard_count\": %u,\n"
            "  \"artifacts\": {\n"
            "    \"tids\":   {\"sha256\": \"%s\", \"size_bytes\": %lld},\n"
            "    \"shards\": {\"sha256\": \"%s\", \"size_bytes\": %lld}\n"
            "  }\n"
            "}\n",
            m->pg_cuvs_version, m->cuvs_version,
            m->database_oid, m->table_oid, m->index_oid, m->relfilenode,
            m->base_generation, m->metric_name, m->dim,
            (long long)m->vector_count, (long long)m->build_timestamp,
            m->shard_count,
            m->tids_sha256,   (long long)m->tids_size_bytes,
            m->shards_sha256, (long long)m->shards_size_bytes);
    } else {
        snprintf(out, out_sz,
            "{\n"
            "  \"pg_cuvs_version\": \"%s\",\n"
            "  \"cuvs_version\": \"%s\",\n"
            "  \"database_oid\": %u,\n"
            "  \"table_oid\": %u,\n"
            "  \"index_oid\": %u,\n"
            "  \"relfilenode\": %u,\n"
            "  \"base_generation\": %u,\n"
            "  \"metric\": \"%s\",\n"
            "  \"dim\": %u,\n"
            "  \"vector_count\": %lld,\n"
            "  \"build_timestamp\": %lld,\n"
            "  \"shard_count\": %u,\n"
            "  \"artifacts\": {\n"
            "    \"cagra\": {\"sha256\": \"%s\", \"size_bytes\": %lld},\n"
            "    \"tids\":  {\"sha256\": \"%s\", \"size_bytes\": %lld}\n"
            "  }\n"
            "}\n",
            m->pg_cuvs_version, m->cuvs_version,
            m->database_oid, m->table_oid, m->index_oid, m->relfilenode,
            m->base_generation, m->metric_name, m->dim,
            (long long)m->vector_count, (long long)m->build_timestamp,
            m->shard_count,
            m->cagra_sha256, (long long)m->cagra_size_bytes,
            m->tids_sha256,  (long long)m->tids_size_bytes);
    }
}

static int
manifest_from_json(const char *json, CuvsManifest *m)
{
    const char *tids;
    const char *cagra;
    const char *shards;

    memset(m, 0, sizeof(*m));
    if (json_get_str(json,  "pg_cuvs_version", m->pg_cuvs_version,
                     sizeof(m->pg_cuvs_version)) != 0)   return -1;
    /* cuvs_version: optional for back-compat with pre-3C-cert manifests; a
     * missing value stays "" and is rejected by the load-time version gate. */
    json_get_str(json, "cuvs_version", m->cuvs_version, sizeof(m->cuvs_version));
    if (json_get_u32(json,  "relfilenode",    &m->relfilenode)     != 0) return -1;
    if (json_get_u32(json,  "database_oid",   &m->database_oid)    != 0) return -1;
    if (json_get_u32(json,  "table_oid",      &m->table_oid)       != 0) return -1;
    if (json_get_u32(json,  "index_oid",      &m->index_oid)       != 0) return -1;
    if (json_get_u32(json,  "base_generation",&m->base_generation)  != 0) return -1;
    if (json_get_str(json,  "metric",          m->metric_name,
                     sizeof(m->metric_name))   != 0)      return -1;
    if (json_get_u32(json,  "dim",            &m->dim)              != 0) return -1;
    if (json_get_i64(json,  "vector_count",   &m->vector_count)     != 0) return -1;
    if (json_get_i64(json,  "build_timestamp",&m->build_timestamp)  != 0) return -1;

    /* shard_count: default 0 if absent (back-compat with pre-3G.2 manifests) */
    m->shard_count = 0;
    json_get_u32(json, "shard_count", &m->shard_count);

    if (m->shard_count >= 2) {
        /* Sharded: require tids + shards; no cagra block */
        tids   = json_find_obj(json, "tids");
        shards = json_find_obj(json, "shards");
        if (!tids || !shards) return -1;

        if (json_get_str(tids,   "sha256",     m->tids_sha256,
                         sizeof(m->tids_sha256))   != 0)  return -1;
        if (json_get_i64(tids,   "size_bytes", &m->tids_size_bytes)   != 0) return -1;
        if (json_get_str(shards, "sha256",     m->shards_sha256,
                         sizeof(m->shards_sha256)) != 0)  return -1;
        if (json_get_i64(shards, "size_bytes", &m->shards_size_bytes) != 0) return -1;
    } else {
        /* Unsharded: require cagra + tids (original behavior) */
        cagra = json_find_obj(json, "cagra");
        tids  = json_find_obj(json, "tids");
        if (!cagra || !tids) return -1;

        if (json_get_str(cagra, "sha256",     m->cagra_sha256,
                         sizeof(m->cagra_sha256)) != 0)       return -1;
        if (json_get_i64(cagra, "size_bytes", &m->cagra_size_bytes) != 0) return -1;
        if (json_get_str(tids,  "sha256",     m->tids_sha256,
                         sizeof(m->tids_sha256))  != 0)       return -1;
        if (json_get_i64(tids,  "size_bytes", &m->tids_size_bytes)  != 0) return -1;
    }
    return 0;
}

/* ----------------------------------------------------------------
 * GCS path helpers
 * ---------------------------------------------------------------- */

/* Versioned prefix: "<prefix>/pg_cuvs/<cluster>/<db>/<idx>/<ts>" */
static void
build_versioned_prefix(const char *bucket_prefix, const char *cluster_id,
                       uint32_t db_oid, uint32_t index_oid,
                       int64_t build_ts, char *out, size_t out_sz)
{
    if (bucket_prefix && bucket_prefix[0])
        snprintf(out, out_sz, "%s/pg_cuvs/%s/%u/%u/%lld",
                 bucket_prefix, cluster_id, db_oid, index_oid,
                 (long long)build_ts);
    else
        snprintf(out, out_sz, "pg_cuvs/%s/%u/%u/%lld",
                 cluster_id, db_oid, index_oid, (long long)build_ts);
}

/* Latest alias prefix (no timestamp) */
static void
build_latest_prefix(const char *bucket_prefix, const char *cluster_id,
                    uint32_t db_oid, uint32_t index_oid,
                    char *out, size_t out_sz)
{
    if (bucket_prefix && bucket_prefix[0])
        snprintf(out, out_sz, "%s/pg_cuvs/%s/%u/%u/latest",
                 bucket_prefix, cluster_id, db_oid, index_oid);
    else
        snprintf(out, out_sz, "pg_cuvs/%s/%u/%u/latest",
                 cluster_id, db_oid, index_oid);
}

/* ----------------------------------------------------------------
 * Public API: upload
 * ---------------------------------------------------------------- */

int
cuvs_objstore_upload(
    const char *snapshot_uri,
    const char *cluster_id,
    const char *gcs_key_file,
    const char *cagra_path,
    const char *tids_path,
    uint32_t    db_oid,
    uint32_t    table_oid,
    uint32_t    index_oid,
    uint32_t    relfilenode,
    uint32_t    metric,
    uint32_t    dim,
    int64_t     vector_count,
    uint32_t    base_generation)
{
    if (!snapshot_uri || snapshot_uri[0] == '\0')
        return 0;

    char bucket[256] = "", prefix[512] = "";
    if (parse_gcs_uri(snapshot_uri, bucket, sizeof(bucket),
                      prefix, sizeof(prefix)) != 0) {
        LOG_WARN("[objstore] invalid snapshot_uri: %s\n", snapshot_uri);
        return -1;
    }

    char token[2048];
    if (get_gcs_token(gcs_key_file, token, sizeof(token)) != 0)
        return -1;

    /* Compute SHA256 and sizes before upload */
    char cagra_sha[65], tids_sha[65];
    if (cuvs_sha256_file(cagra_path, cagra_sha) != 0 ||
        cuvs_sha256_file(tids_path,  tids_sha)  != 0) {
        LOG_WARN("[objstore] SHA256 computation failed for %u/%u\n",
                 db_oid, index_oid);
        return -1;
    }

    struct stat st;
    int64_t cagra_sz = (stat(cagra_path, &st) == 0) ? (int64_t)st.st_size : -1;
    int64_t tids_sz  = (stat(tids_path,  &st) == 0) ? (int64_t)st.st_size : -1;

    /* Build manifest */
    int64_t build_ts = (int64_t)time(NULL);
    CuvsManifest m;
    memset(&m, 0, sizeof(m));
    strncpy(m.pg_cuvs_version, PG_CUVS_VERSION,   sizeof(m.pg_cuvs_version) - 1);
    strncpy(m.cuvs_version,    CUVS_BUILD_VERSION, sizeof(m.cuvs_version) - 1);
    m.database_oid     = db_oid;
    m.table_oid        = table_oid;
    m.index_oid        = index_oid;
    m.relfilenode      = relfilenode;
    m.base_generation  = base_generation;
    m.dim              = dim;
    m.vector_count     = vector_count;
    m.build_timestamp  = build_ts;
    m.cagra_size_bytes = cagra_sz;
    m.tids_size_bytes  = tids_sz;
    memcpy(m.cagra_sha256, cagra_sha, sizeof(m.cagra_sha256));
    memcpy(m.tids_sha256,  tids_sha,  sizeof(m.tids_sha256));

    const char *metric_names[] = {"l2", "cosine", "ip"};
    strncpy(m.metric_name,
            (metric < 3) ? metric_names[metric] : "l2",
            sizeof(m.metric_name) - 1);

    char manifest_json[4096];
    manifest_to_json(&m, manifest_json, sizeof(manifest_json));

    char vprefix[1024], lprefix[1024], obj[1280];
    build_versioned_prefix(prefix, cluster_id, db_oid, index_oid,
                           build_ts, vprefix, sizeof(vprefix));
    build_latest_prefix(prefix, cluster_id, db_oid, index_oid,
                        lprefix, sizeof(lprefix));

    /* Upload artifacts first (manifest-last for atomicity) */
    snprintf(obj, sizeof(obj), "%s/index.cagra", vprefix);
    if (gcs_upload_file(bucket, obj, cagra_path, token) != 0) {
        LOG_WARN("[objstore] upload .cagra FAILED for %u/%u\n", db_oid, index_oid);
        return -1;
    }
    LOG_INFO("[objstore] uploaded %s\n", obj);

    snprintf(obj, sizeof(obj), "%s/index.tids", vprefix);
    if (gcs_upload_file(bucket, obj, tids_path, token) != 0) {
        LOG_WARN("[objstore] upload .tids FAILED for %u/%u\n", db_oid, index_oid);
        return -1;
    }
    LOG_INFO("[objstore] uploaded %s\n", obj);

    /* Versioned manifest — the authoritative record */
    snprintf(obj, sizeof(obj), "%s/manifest.json", vprefix);
    if (gcs_upload_string(bucket, obj, manifest_json, token) != 0) {
        LOG_WARN("[objstore] upload versioned manifest FAILED for %u/%u\n",
                 db_oid, index_oid);
        return -1;
    }

    /* Latest alias — convenience pointer (overwritten by newer builds) */
    snprintf(obj, sizeof(obj), "%s/manifest.json", lprefix);
    if (gcs_upload_string(bucket, obj, manifest_json, token) != 0)
        LOG_WARN("[objstore] upload latest manifest FAILED for %u/%u "
                 "(non-fatal; versioned copy is intact)\n", db_oid, index_oid);

    LOG_INFO("[objstore] snapshot complete %u/%u ts=%lld relfilenode=%u\n",
             db_oid, index_oid, (long long)build_ts, relfilenode);
    return 0;
}

/* ----------------------------------------------------------------
 * Public API: upload (sharded)
 * ---------------------------------------------------------------- */

int
cuvs_objstore_upload_sharded(
    const char *snapshot_uri,
    const char *cluster_id,
    const char *gcs_key_file,
    const char *index_dir,
    uint32_t    db_oid,
    uint32_t    table_oid,
    uint32_t    index_oid,
    uint32_t    relfilenode,
    uint32_t    metric,
    uint32_t    dim,
    int64_t     vector_count,
    uint32_t    base_generation,
    uint32_t    shard_count)
{
    char     bucket[256] = "", prefix[512] = "";
    char     token[2048];
    char     tids_path[1024], shards_path[1024], shard_path[1024];
    char     tids_sha[65], shards_sha[65];
    char     vprefix[1024], lprefix[1024], obj[1280];
    char     manifest_json[4096];
    int64_t  build_ts;
    struct   stat st;
    uint32_t s;
    CuvsManifest m;
    const char *metric_names[] = {"l2", "cosine", "ip"};

    if (!snapshot_uri || snapshot_uri[0] == '\0')
        return 0;

    if (parse_gcs_uri(snapshot_uri, bucket, sizeof(bucket),
                      prefix, sizeof(prefix)) != 0) {
        LOG_WARN("[objstore] invalid snapshot_uri: %s\n", snapshot_uri);
        return -1;
    }

    if (get_gcs_token(gcs_key_file, token, sizeof(token)) != 0)
        return -1;

    /* Construct local paths */
    snprintf(tids_path,   sizeof(tids_path),   "%s/%u_%u.tids",   index_dir, db_oid, index_oid);
    snprintf(shards_path, sizeof(shards_path), "%s/%u_%u.shards", index_dir, db_oid, index_oid);

    /* SHA256 + size of .tids and .shards */
    if (cuvs_sha256_file(tids_path,   tids_sha)   != 0 ||
        cuvs_sha256_file(shards_path, shards_sha) != 0) {
        LOG_WARN("[objstore] SHA256 computation failed for %u/%u (sharded)\n",
                 db_oid, index_oid);
        return -1;
    }

    int64_t tids_sz   = (stat(tids_path,   &st) == 0) ? (int64_t)st.st_size : -1;
    int64_t shards_sz = (stat(shards_path, &st) == 0) ? (int64_t)st.st_size : -1;

    /* Build manifest */
    build_ts = (int64_t)time(NULL);
    memset(&m, 0, sizeof(m));
    strncpy(m.pg_cuvs_version, PG_CUVS_VERSION,   sizeof(m.pg_cuvs_version) - 1);
    strncpy(m.cuvs_version,    CUVS_BUILD_VERSION, sizeof(m.cuvs_version) - 1);
    m.database_oid      = db_oid;
    m.table_oid         = table_oid;
    m.index_oid         = index_oid;
    m.relfilenode       = relfilenode;
    m.base_generation   = base_generation;
    m.dim               = dim;
    m.vector_count      = vector_count;
    m.build_timestamp   = build_ts;
    m.shard_count       = shard_count;
    m.tids_size_bytes   = tids_sz;
    m.shards_size_bytes = shards_sz;
    memcpy(m.tids_sha256,   tids_sha,   sizeof(m.tids_sha256));
    memcpy(m.shards_sha256, shards_sha, sizeof(m.shards_sha256));
    strncpy(m.metric_name,
            (metric < 3) ? metric_names[metric] : "l2",
            sizeof(m.metric_name) - 1);

    manifest_to_json(&m, manifest_json, sizeof(manifest_json));

    build_versioned_prefix(prefix, cluster_id, db_oid, index_oid,
                           build_ts, vprefix, sizeof(vprefix));
    build_latest_prefix(prefix, cluster_id, db_oid, index_oid,
                        lprefix, sizeof(lprefix));

    /* Upload all shard .cagra files first */
    for (s = 0; s < shard_count; s++) {
        snprintf(shard_path, sizeof(shard_path),
                 "%s/%u_%u.s%03u.cagra", index_dir, db_oid, index_oid, s);
        snprintf(obj, sizeof(obj), "%s/index.s%03u.cagra", vprefix, s);
        if (gcs_upload_file(bucket, obj, shard_path, token) != 0) {
            LOG_WARN("[objstore] upload shard %u .cagra FAILED for %u/%u\n",
                     s, db_oid, index_oid);
            return -1;
        }
        LOG_INFO("[objstore] uploaded %s\n", obj);
    }

    /* Upload .tids */
    snprintf(obj, sizeof(obj), "%s/index.tids", vprefix);
    if (gcs_upload_file(bucket, obj, tids_path, token) != 0) {
        LOG_WARN("[objstore] upload .tids FAILED for %u/%u\n", db_oid, index_oid);
        return -1;
    }
    LOG_INFO("[objstore] uploaded %s\n", obj);

    /* Upload .shards manifest */
    snprintf(obj, sizeof(obj), "%s/index.shards", vprefix);
    if (gcs_upload_file(bucket, obj, shards_path, token) != 0) {
        LOG_WARN("[objstore] upload .shards FAILED for %u/%u\n", db_oid, index_oid);
        return -1;
    }
    LOG_INFO("[objstore] uploaded %s\n", obj);

    /* Versioned manifest — the authoritative record (uploaded last) */
    snprintf(obj, sizeof(obj), "%s/manifest.json", vprefix);
    if (gcs_upload_string(bucket, obj, manifest_json, token) != 0) {
        LOG_WARN("[objstore] upload versioned manifest FAILED for %u/%u\n",
                 db_oid, index_oid);
        return -1;
    }

    /* Latest alias */
    snprintf(obj, sizeof(obj), "%s/manifest.json", lprefix);
    if (gcs_upload_string(bucket, obj, manifest_json, token) != 0)
        LOG_WARN("[objstore] upload latest manifest FAILED for %u/%u "
                 "(non-fatal; versioned copy is intact)\n", db_oid, index_oid);

    LOG_INFO("[objstore] sharded snapshot complete %u/%u shards=%u ts=%lld relfilenode=%u\n",
             db_oid, index_oid, shard_count, (long long)build_ts, relfilenode);
    return 0;
}

/* ----------------------------------------------------------------
 * Public API: download
 * ---------------------------------------------------------------- */

int
cuvs_objstore_download(
    const char *snapshot_uri,
    const char *cluster_id,
    const char *gcs_key_file,
    const char *index_dir,
    uint32_t    db_oid,
    uint32_t    index_oid,
    uint32_t    local_relfilenode,
    uint64_t   *build_timestamp_out)
{
    if (!snapshot_uri || snapshot_uri[0] == '\0')
        return -1;

    char bucket[256] = "", prefix[512] = "";
    if (parse_gcs_uri(snapshot_uri, bucket, sizeof(bucket),
                      prefix, sizeof(prefix)) != 0)
        return -1;

    char token[2048];
    if (get_gcs_token(gcs_key_file, token, sizeof(token)) != 0)
        return -1;

    /* Fetch the latest-alias manifest */
    char lprefix[1024], obj[1280];
    build_latest_prefix(prefix, cluster_id, db_oid, index_oid,
                        lprefix, sizeof(lprefix));
    snprintf(obj, sizeof(obj), "%s/manifest.json", lprefix);

    char *manifest_json = gcs_fetch_string(bucket, obj, token);
    if (!manifest_json) {
        LOG_DEBUG("[objstore] no GCS manifest for %u/%u — not cached\n",
                  db_oid, index_oid);
        return -1;
    }

    CuvsManifest m;
    if (manifest_from_json(manifest_json, &m) != 0) {
        LOG_WARN("[objstore] corrupt manifest for %u/%u\n", db_oid, index_oid);
        free(manifest_json);
        return -1;
    }
    free(manifest_json);

    /* ---- HARD REJECT on heap incompatibility ---- */
    if (local_relfilenode != 0 && m.relfilenode != local_relfilenode) {
        LOG_WARN("[objstore] HEAP COMPAT MISMATCH %u/%u: "
                 "manifest relfilenode=%u local=%u — artifact NOT loaded. "
                 "REINDEX is required to rebuild from this node's heap.\n",
                 db_oid, index_oid, m.relfilenode, local_relfilenode);
        return -1;
    }

    /* Sanity: OIDs must match (protects against bucket prefix collisions) */
    if (m.database_oid != db_oid || m.index_oid != index_oid) {
        LOG_WARN("[objstore] manifest OID mismatch %u/%u: "
                 "manifest has db=%u idx=%u\n",
                 db_oid, index_oid, m.database_oid, m.index_oid);
        return -1;
    }

    /* ---- Version compatibility gate (ADR-013) ----
     * A CAGRA artifact serialized by a different cuVS is not guaranteed to
     * deserialize safely (serialization format is cuVS-version-coupled). Hard
     * reject on cuVS mismatch — the node must REINDEX from its local heap. An
     * empty manifest cuvs_version (pre-cert artifact) is treated as unknown and
     * rejected, fail-closed. pg_cuvs drift is a warning only. */
    if (strcmp(m.cuvs_version, CUVS_BUILD_VERSION) != 0) {
        LOG_WARN("[objstore] cuVS VERSION MISMATCH %u/%u: manifest cuvs=%s "
                 "running=%s — artifact NOT loaded; REINDEX required.\n",
                 db_oid, index_oid,
                 m.cuvs_version[0] ? m.cuvs_version : "(none)", CUVS_BUILD_VERSION);
        return -1;
    }
    if (strcmp(m.pg_cuvs_version, PG_CUVS_VERSION) != 0)
        LOG_WARN("[objstore] pg_cuvs version drift %u/%u: manifest=%s running=%s "
                 "(loading anyway)\n",
                 db_oid, index_oid, m.pg_cuvs_version, PG_CUVS_VERSION);

    /* Reconstruct the versioned prefix from the manifest's build_timestamp */
    char vprefix[1024];
    build_versioned_prefix(prefix, cluster_id, db_oid, index_oid,
                           m.build_timestamp, vprefix, sizeof(vprefix));

    LOG_INFO("[objstore] downloading %u/%u from GCS "
             "(ts=%lld relfilenode=%u shards=%u)\n",
             db_oid, index_oid,
             (long long)m.build_timestamp, m.relfilenode, m.shard_count);

    if (m.shard_count >= 2) {
        /* ---- Sharded download path ---- */
        char tids_path[1024], shards_path[1024], shard_path[1024];
        char got_sha[65];
        uint32_t s;

        snprintf(tids_path,   sizeof(tids_path),
                 "%s/%u_%u.tids",   index_dir, db_oid, index_oid);
        snprintf(shards_path, sizeof(shards_path),
                 "%s/%u_%u.shards", index_dir, db_oid, index_oid);

        /* Download .tids first */
        snprintf(obj, sizeof(obj), "%s/index.tids", vprefix);
        if (gcs_download_file(bucket, obj, tids_path, token) != 0) {
            LOG_WARN("[objstore] download .tids FAILED for %u/%u (sharded)\n",
                     db_oid, index_oid);
            return -1;
        }

        /* Verify .tids SHA256 */
        if (cuvs_sha256_file(tids_path, got_sha) != 0 ||
            strcmp(got_sha, m.tids_sha256) != 0) {
            LOG_WARN("[objstore] .tids SHA256 MISMATCH %u/%u "
                     "(got=%s expected=%s) — discarding\n",
                     db_oid, index_oid, got_sha, m.tids_sha256);
            unlink(tids_path);
            return -1;
        }

        /* Download .shards */
        snprintf(obj, sizeof(obj), "%s/index.shards", vprefix);
        if (gcs_download_file(bucket, obj, shards_path, token) != 0) {
            LOG_WARN("[objstore] download .shards FAILED for %u/%u\n",
                     db_oid, index_oid);
            unlink(tids_path);
            return -1;
        }

        /* Verify .shards SHA256 */
        if (cuvs_sha256_file(shards_path, got_sha) != 0 ||
            strcmp(got_sha, m.shards_sha256) != 0) {
            LOG_WARN("[objstore] .shards SHA256 MISMATCH %u/%u "
                     "(got=%s expected=%s) — discarding\n",
                     db_oid, index_oid, got_sha, m.shards_sha256);
            unlink(tids_path);
            unlink(shards_path);
            return -1;
        }

        /* Download each shard .cagra (integrity verified at load via .shards CRCs) */
        for (s = 0; s < m.shard_count; s++) {
            snprintf(shard_path, sizeof(shard_path),
                     "%s/%u_%u.s%03u.cagra", index_dir, db_oid, index_oid, s);
            snprintf(obj, sizeof(obj), "%s/index.s%03u.cagra", vprefix, s);
            if (gcs_download_file(bucket, obj, shard_path, token) != 0) {
                uint32_t k;
                LOG_WARN("[objstore] download shard %u .cagra FAILED for %u/%u\n",
                         s, db_oid, index_oid);
                unlink(tids_path);
                unlink(shards_path);
                for (k = 0; k < s; k++) {
                    snprintf(shard_path, sizeof(shard_path),
                             "%s/%u_%u.s%03u.cagra", index_dir, db_oid, index_oid, k);
                    unlink(shard_path);
                }
                return -1;
            }
        }

        LOG_INFO("[objstore] sharded download verified OK for %u/%u "
                 "(%u shards)\n", db_oid, index_oid, m.shard_count);
    } else {
        /* ---- Unsharded download path (original behavior) ---- */
        char cagra_path[1024], tids_path[1024];
        char got_sha[65];

        snprintf(cagra_path, sizeof(cagra_path),
                 "%s/%u_%u.cagra", index_dir, db_oid, index_oid);
        snprintf(tids_path, sizeof(tids_path),
                 "%s/%u_%u.tids",  index_dir, db_oid, index_oid);

        /* Download .tids first (smaller — fail fast on auth/connectivity issues) */
        snprintf(obj, sizeof(obj), "%s/index.tids", vprefix);
        if (gcs_download_file(bucket, obj, tids_path, token) != 0) {
            LOG_WARN("[objstore] download .tids FAILED for %u/%u\n",
                     db_oid, index_oid);
            return -1;
        }

        /* Verify .tids SHA256 before proceeding to the large .cagra download */
        if (cuvs_sha256_file(tids_path, got_sha) != 0 ||
            strcmp(got_sha, m.tids_sha256) != 0) {
            LOG_WARN("[objstore] .tids SHA256 MISMATCH %u/%u "
                     "(got=%s expected=%s) — discarding\n",
                     db_oid, index_oid, got_sha, m.tids_sha256);
            unlink(tids_path);
            return -1;
        }

        /* Download .cagra (may be several GB — generous timeout set in helper) */
        snprintf(obj, sizeof(obj), "%s/index.cagra", vprefix);
        if (gcs_download_file(bucket, obj, cagra_path, token) != 0) {
            LOG_WARN("[objstore] download .cagra FAILED for %u/%u\n",
                     db_oid, index_oid);
            unlink(tids_path);
            return -1;
        }

        /* Verify .cagra SHA256 */
        if (cuvs_sha256_file(cagra_path, got_sha) != 0 ||
            strcmp(got_sha, m.cagra_sha256) != 0) {
            LOG_WARN("[objstore] .cagra SHA256 MISMATCH %u/%u "
                     "(got=%s expected=%s) — discarding\n",
                     db_oid, index_oid, got_sha, m.cagra_sha256);
            unlink(tids_path);
            unlink(cagra_path);
            return -1;
        }

        LOG_INFO("[objstore] download verified OK for %u/%u\n", db_oid, index_oid);
    }

    if (build_timestamp_out)
        *build_timestamp_out = (uint64_t)m.build_timestamp;
    return 0;
}
