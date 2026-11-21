/*
 *
 * DATUM Gateway
 * Decentralized Alternative Templates for Universal Mining
 *
 * This file is part of CONVOY's Bitcoin mining decentralization
 * project, DATUM.
 *
 * https://convoy.xyz
 *
 * ---
 *
 * Copyright (c) 2024-2026 Bitcoin Ocean, LLC, Jason Hughes, and individual contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <jansson.h>
#include <stdbool.h>

#include "datum_conf.h"
#include "datum_jsonrpc.h"
#include "datum_utils.h"

static char *gbt_cache_prev = NULL;
static char *gbt_cache_txns = NULL;
static size_t gbt_cache_txns_len = 0;
static json_t *gbt_cache_result = NULL;

static uint64_t gbt_hits = 0;
static uint64_t gbt_misses = 0;
static uint64_t gbt_stat_ts = 0;

// TODO: Clean this up.  Most of this is very old code from other parts of Eligius/OCEAN internal tools and needs
// a solid makeover.
// However, it's all quite functional, so not a top priority.

static void databuf_free(struct data_buffer *db) {
	if (!db) {
		return;
	}
	
	free(db->buf);
	memset(db, 0, sizeof(*db));
}

static size_t all_data_cb(const void *ptr, size_t size, size_t nmemb, void *user_data) {
	struct data_buffer *db = user_data;
	size_t len, oldlen, newlen;
	void *newmem;
	
	if (SIZE_MAX / size < nmemb) abort();
	len = size * nmemb;
	
	oldlen = db->len;
	if (SIZE_MAX - oldlen < len) abort();
	newlen = oldlen + len;
	
	newmem = realloc(db->buf, newlen + 1);
	if (!newmem) {
		return 0;
	}
	
	db->buf = newmem;
	db->len = newlen;
	memcpy(&((char *)db->buf)[oldlen], ptr, len);
	((char *)db->buf)[newlen] = 0;
	
	return len;
}

static size_t upload_data_cb(void *ptr, size_t size, size_t nmemb, void *user_data) {
	struct upload_buffer *ub = user_data;
	size_t len;
	if (SIZE_MAX / size < nmemb) nmemb = SIZE_MAX / size;
	len = size * nmemb;
	
	if (len > ub->len) len = ub->len;
	
	if (len) {
		memcpy(ptr, ub->buf, len);
		ub->buf = &((const char *)ub->buf)[len];
		ub->len -= len;
	}
	
	return len;
}

/* Find a JSON string value for key. Writes start/end of the quoted value. */
static bool find_string_value(const char *buf, size_t len, const char *key,
                              const char **start, const char **end) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(buf, pat);
	if (!p || p >= buf + len) return false;
	p += strlen(pat);
	while (p < buf + len && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
	if (p >= buf + len || *p != '"') return false;
	p++;
	*start = p;
	while (p < buf + len && *p != '"') {
		if (*p == '\\' && p + 1 < buf + len) p++;
		p++;
	}
	if (p >= buf + len) return false;
	*end = p;
	return true;
}

/* Find the raw "transactions" array. */
static bool find_txns_span(const char *buf, size_t len,
                           const char **start, const char **end) {
	const char *p = strstr(buf, "\"transactions\"");
	if (!p || p >= buf + len) return false;
	p += 14;
	while (p < buf + len && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
	if (p >= buf + len || *p != '[') return false;
	*start = p;
	int depth = 0;
	bool in_str = false, esc = false;
	for (; p < buf + len; p++) {
		char c = *p;
		if (in_str) {
			if (esc) { esc = false; continue; }
			if (c == '\\') { esc = true; continue; }
			if (c == '"') in_str = false;
			continue;
		}
		if (c == '"') { in_str = true; continue; }
		if (c == '[' || c == '{') depth++;
		else if (c == ']' || c == '}') {
			depth--;
			if (depth == 0) { *end = p + 1; return true; }
		}
	}
	return false;
}

static void gbt_note(bool hit) {
	if (hit) gbt_hits++; else gbt_misses++;
	uint64_t now = current_time_millis();
	if (gbt_stat_ts == 0) gbt_stat_ts = now;
	if (now - gbt_stat_ts >= 60000) {
		uint64_t total = gbt_hits + gbt_misses;
		DLOG_INFO("GBT cache: %llu hits, %llu misses (%.1f%%) in last minute",
		          (unsigned long long)gbt_hits,
		          (unsigned long long)gbt_misses,
		          total ? (100.0 * gbt_hits / total) : 0.0);
		gbt_hits = 0;
		gbt_misses = 0;
		gbt_stat_ts = now;
	}
}

/* Find the raw "result" value. Returns start/end pointers into buf.
 * Handles a JSON object or null. Returns false if not found. */
static bool find_result_span(const char *buf, size_t len,
                             const char **start, const char **end) {
	const char *p = buf;
	const char *limit = buf + len;
	const char *key = strstr(p, "\"result\"");
	if (!key || key >= limit) return false;
	p = key + 8;
	while (p < limit && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
	if (p >= limit) return false;
	*start = p;
	if (*p == 'n') { /* null */
		if (p + 4 <= limit && memcmp(p, "null", 4) == 0) {
			*end = p + 4;
			return true;
		}
		return false;
	}
	if (*p != '{') return false;
	int depth = 0;
	bool in_str = false;
	bool esc = false;
	for (; p < limit; p++) {
		char c = *p;
		if (in_str) {
			if (esc) { esc = false; continue; }
			if (c == '\\') { esc = true; continue; }
			if (c == '"') in_str = false;
			continue;
		}
		if (c == '"') { in_str = true; continue; }
		if (c == '{') depth++;
		else if (c == '}') {
			depth--;
			if (depth == 0) { *end = p + 1; return true; }
		}
	}
	return false;
}

/* Pull the numeric id out of the raw response. */
static json_int_t extract_id(const char *buf, size_t len) {
	const char *p = strstr(buf, "\"id\"");
	if (!p || p >= buf + len) return 0;
	p += 4;
	while (p < buf + len && (*p == ' ' || *p == '\t' || *p == ':' )) p++;
	return (json_int_t)strtoll(p, NULL, 10);
}

json_t *bitcoind_json_rpc_call_cached(CURL * const curl, global_config_t * const cfg, const char * const rpc_req) {
	struct data_buffer all_data = {0};
	struct upload_buffer upload_data = {0};
	json_t *val = NULL;
	json_error_t err;
	int rc;
	struct curl_slist *headers = NULL;
	char curl_err_str[CURL_ERROR_SIZE];
	const char *prev_s = NULL, *prev_e = NULL;
	const char *tx_s = NULL, *tx_e = NULL;

	curl_easy_setopt(curl, CURLOPT_URL, cfg->bitcoind_rpcurl);
	curl_easy_setopt(curl, CURLOPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, all_data_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &all_data);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, upload_data_cb);
	curl_easy_setopt(curl, CURLOPT_READDATA, &upload_data);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err_str);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	if (cfg->bitcoind_rpcuserpass[0]) {
		curl_easy_setopt(curl, CURLOPT_USERPWD, cfg->bitcoind_rpcuserpass);
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
	}
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	upload_data.buf = rpc_req;
	upload_data.len = strlen(rpc_req);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)upload_data.len);
	headers = curl_slist_append(headers, "Content-type: application/json");
	headers = curl_slist_append(headers, "Expect:");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	rc = curl_easy_perform(curl);
	if (rc) {
		DLOG_DEBUG("json_rpc_call: HTTP request failed: %s", curl_err_str);
		goto err_out;
	}

	bool have_prev = find_string_value(all_data.buf, all_data.len, "previousblockhash", &prev_s, &prev_e);
	bool have_tx   = find_txns_span(all_data.buf, all_data.len, &tx_s, &tx_e);

	if (have_prev && have_tx && gbt_cache_result && gbt_cache_prev && gbt_cache_txns &&
	    (size_t)(prev_e - prev_s) == strlen(gbt_cache_prev) &&
	    memcmp(prev_s, gbt_cache_prev, prev_e - prev_s) == 0 &&
	    (size_t)(tx_e - tx_s) == gbt_cache_txns_len &&
	    memcmp(tx_s, gbt_cache_txns, gbt_cache_txns_len) == 0) {
		/* Hit: same tip, same tx set. */
		gbt_note(true);
		json_t *wrap = json_object();
		json_object_set(wrap, "result", gbt_cache_result);
		json_object_set_new(wrap, "error", json_null());
		json_object_set_new(wrap, "id", json_integer(0));
		databuf_free(&all_data);
		curl_slist_free_all(headers);
		curl_easy_reset(curl);
		return wrap;
	}

	gbt_note(false);

	val = JSON_LOADS(all_data.buf, &err);
	if (!val) {
		DLOG_DEBUG("JSON decode failed(%d): %s", err.line, err.text);
		goto err_out;
	}

	if (have_prev && have_tx) {
		size_t pn = (size_t)(prev_e - prev_s);
		size_t tn = (size_t)(tx_e - tx_s);
		char *np = malloc(pn + 1);
		char *nt = malloc(tn);
		if (np && nt) {
			memcpy(np, prev_s, pn); np[pn] = 0;
			memcpy(nt, tx_s, tn);
			free(gbt_cache_prev); gbt_cache_prev = np;
			free(gbt_cache_txns); gbt_cache_txns = nt;
			gbt_cache_txns_len = tn;
		} else {
			free(np); free(nt);
		}
		json_t *res = json_object_get(val, "result");
		if (res) {
			if (gbt_cache_result) json_decref(gbt_cache_result);
			gbt_cache_result = json_incref(res);
		}
	}

	databuf_free(&all_data);
	curl_slist_free_all(headers);
	curl_easy_reset(curl);
	return val;

err_out:
	databuf_free(&all_data);
	curl_slist_free_all(headers);
	curl_easy_reset(curl);
	return NULL;
}

char *basic_http_call(CURL *curl, const char *url) {
	CURLcode rc;
	struct data_buffer all_data = { };
	char curl_err_str[CURL_ERROR_SIZE];
	char *out;
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, all_data_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &all_data);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err_str);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // quick timeout!
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // quick timeout!
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	
	rc = curl_easy_perform(curl);
	if (rc) {
		DLOG_DEBUG("HTTP request failed: %s", curl_err_str);
		goto err_out;
	}
	
	out = calloc(strlen(all_data.buf)+20,1);
	if (!out) goto err_out;
	
	strcpy(out, all_data.buf);
	
	databuf_free(&all_data);
	curl_easy_reset(curl);
	return out;

err_out:
	databuf_free(&all_data);
	curl_easy_reset(curl);
	return NULL;
}

json_t *json_rpc_call_full(CURL *curl, const char *url, const char *userpass, const char *rpc_req, const char *extra_header, long * const http_resp_code_out) {
	json_t *val, *err_val, *res_val;
	CURLcode rc;
	struct data_buffer all_data = { };
	struct upload_buffer upload_data;
	json_error_t err = { };
	struct curl_slist *headers = NULL;
	char curl_err_str[CURL_ERROR_SIZE];
	bool check_for_result = true;
	
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_ENCODING, "");
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, all_data_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &all_data);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, upload_data_cb);
	curl_easy_setopt(curl, CURLOPT_READDATA, &upload_data);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_err_str);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L); // quick timeout!
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L); // quick timeout!
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	
	if (userpass) {
		curl_easy_setopt(curl, CURLOPT_USERPWD, userpass);
		curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
	}
	
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	
	upload_data.buf = rpc_req;
	upload_data.len = strlen(rpc_req);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)upload_data.len);
	
	headers = curl_slist_append(headers, "Content-type: application/json");
	headers = curl_slist_append(headers, "Expect:");
	
	if (extra_header) {
		headers = curl_slist_append(headers, extra_header);
		check_for_result = false;
	}
	
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	
	rc = curl_easy_perform(curl);
	if (rc) {
		if (http_resp_code_out) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, http_resp_code_out);
		DLOG_DEBUG("json_rpc_call: HTTP request failed: %s", curl_err_str);
		DLOG_DEBUG("json_rpc_call: Request was: %s",rpc_req);
		goto err_out;
	}
	
	val = JSON_LOADS(all_data.buf, &err);
	if (!val) {
		DLOG_DEBUG("JSON decode failed(%d): %s", err.line, err.text);
		goto err_out;
	}
	
	if (check_for_result) {
		res_val = json_object_get(val, "result");
		err_val = json_object_get(val, "error");
		
		// A JSON-RPC reply's "result" being present-but-null is a legitimate, successful reply
		// (e.g. submitblock/preciousblock return null on success) -- it's the caller's job to
		// decide what a given method's result means, not ours. The only conditions that actually
		// indicate we didn't get a usable reply are: the "result" key being missing outright (a
		// non-compliant response), or a genuine non-null "error".
		const bool has_error = err_val && !json_is_null(err_val);
		const bool missing_result = !res_val;
		
		if (has_error || missing_result) {
			char *s;
			
			if (err_val) {
				s = json_dumps(err_val, JSON_INDENT(3));
			} else {
				s = strdup("(unknown reason)");
			}
			
			DLOG_DEBUG("JSON-RPC call failed: %s", s);
			
			free(s);
			json_decref(val);
			
			goto err_out;
		}
	}
	
	databuf_free(&all_data);
	curl_slist_free_all(headers);
	curl_easy_reset(curl);
	return val;

err_out:
	databuf_free(&all_data);
	curl_slist_free_all(headers);
	curl_easy_reset(curl);
	return NULL;
}

json_t *json_rpc_call(CURL *curl, const char *url, const char *userpass, const char *rpc_req) {
	return json_rpc_call_full(curl, url, userpass, rpc_req, NULL, NULL);
}

bool update_rpc_cookie(global_config_t * const cfg) {
	assert(!cfg->bitcoind_rpcuser[0]);
	FILE * const F = fopen(cfg->bitcoind_rpccookiefile, "r");
	if (!F) {
		DLOG_ERROR("Cannot %s cookie file %s", "open", datum_config.bitcoind_rpccookiefile);
		return false;
	}
	if (!(fgets(cfg->bitcoind_rpcuserpass, sizeof(cfg->bitcoind_rpcuserpass), F) && cfg->bitcoind_rpcuserpass[0])) {
		DLOG_ERROR("Cannot %s cookie file %s", "read", datum_config.bitcoind_rpccookiefile);
		return false;
	}
	return true;
}

void update_rpc_auth(global_config_t * const cfg) {
	if (datum_config.bitcoind_rpccookiefile[0] && !cfg->bitcoind_rpcuser[0]) {
		update_rpc_cookie(cfg);
	} else {
		snprintf(datum_config.bitcoind_rpcuserpass, sizeof(datum_config.bitcoind_rpcuserpass), "%s:%s", datum_config.bitcoind_rpcuser, datum_config.bitcoind_rpcpassword);
	}
}

json_t *bitcoind_json_rpc_call(CURL * const curl, global_config_t * const cfg, const char * const rpc_req) {
	long http_resp_code = -1;
	json_t *j = json_rpc_call_full(curl, cfg->bitcoind_rpcurl, cfg->bitcoind_rpcuserpass, rpc_req, NULL, &http_resp_code);
	if (j) return j;
	if (cfg->bitcoind_rpcuser[0]) return NULL;
	if (http_resp_code != 401) return NULL;
	
	// Authentication failure using cookie; reload cookie file and try again
	if (!update_rpc_cookie(cfg)) return NULL;
	return json_rpc_call(curl, cfg->bitcoind_rpcurl, cfg->bitcoind_rpcuserpass, rpc_req);
}
