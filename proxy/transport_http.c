/*
OpenIO SDS proxy
Copyright (C) 2014 Worldine, original work as part of Redcurrant
Copyright (C) 2015 OpenIO, modified as part of OpenIO Software Defined Storage

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stddef.h>
#include <string.h>
#include <sys/stat.h>

#include <metautils/lib/metautils.h>
#include <server/slab.h>
#include <server/network_server.h>
#include <server/stats_holder.h>

#include "transport_http.h"

struct transport_client_context_s
{
	struct http_parser_s *parser;
	struct http_request_s *request;
	struct http_request_dispatcher_s *dispatcher;
};

struct http_request_handler_s
{
	enum http_rc_e (*handler)(gpointer u,
			struct http_request_s *request, struct http_reply_ctx_s *reply);
};

struct http_request_dispatcher_s
{
	gpointer u;
	GArray *requests;
};

struct req_ctx_s
{
	gint64 tv_start, tv_parsed;

	struct network_client_s *client;
	struct network_transport_s *transport;
	struct transport_client_context_s *context;
	struct http_request_dispatcher_s *dispatcher;
	struct http_request_s *request;

	gchar *uid;

	gboolean close_after_request;
};

static int http_notify_input(struct network_client_s *clt);

//------------------------------------------------------------------------------

enum http_parser_step_e
{
	STEP_FIRST_R0, STEP_FIRST_N0,
	STEP_SEP_R0, STEP_SEP_N0,
	STEP_HEADERS_R0, STEP_HEADERS_N0,
	STEP_BODY_ASIS
};

struct http_parser_s
{
	enum http_parser_step_e step;
	GError *error;
	GString *buf;

	gint64 content_read;
	gint64 content_length;
	void (*command_provider)(const gchar *req, const gchar *sel, const gchar *ver);
	void (*header_provider)(const gchar *name, const gchar *value);
	void (*body_provider)(const guint8 *data, gsize data_len);
};

struct http_parsing_result_s
{
	gsize consumed;
	enum { HPRC_SUCCESS = 0, HPRC_MORE, HPRC_ERROR } status;
};

static void
gstr_chomp(GString *gstr)
{
	if (!gstr)
		return;
	gchar *start, *end;
	start = gstr->str;
	end = start + gstr->len;
	while (end > start && (*(end-1) == '\n' || *(end-1) == '\r'))
		*(--end) = '\0';
}

static gboolean
_manage_and_renew_command(struct http_parser_s *parser, GString *buf)
{
	gchar *cmd, *selector, *version;

	if (!buf->len)
		return FALSE;

	gstr_chomp(buf);
	cmd = buf->str;
	selector = strchr(cmd, ' ');
	if (!selector)
		return FALSE;
	*(selector++) = '\0';
	version = strrchr(selector, ' ');
	if (!version)
		return FALSE;
	*(version++) = '\0';

	if (parser->command_provider)
		parser->command_provider(cmd, selector, version);

	g_string_set_size(buf, 0);
	return TRUE;
}

static gboolean
_manage_and_renew_header(struct http_parser_s *parser, GString *buf)
{
	if (!buf->len)
		return TRUE;

	gstr_chomp(buf);
	gchar *header = buf->str;
	gchar *sep = strchr(header, ':');
	if (!sep)
		return FALSE;
	*(sep++) = '\0';
	if (*(sep++) != ' ')
		return FALSE;

	if (!g_ascii_strcasecmp(header, "Content-Length"))
		parser->content_length = g_ascii_strtoll(sep, NULL, 10);

	if (parser->header_provider)
		parser->header_provider(header, sep);

	g_string_set_size(buf, 0);
	return TRUE;
}

static struct http_parsing_result_s
http_parse(struct http_parser_s *parser, const guint8 *data, gsize available)
{
	gsize consumed = 0;

	struct http_parsing_result_s _build_rc(int status, const gchar *msg) {
		struct http_parsing_result_s rc;
		if (msg)
			parser->error = NEWERROR(0, "%s", msg);
		rc.status = status;
		rc.consumed = consumed;
		return rc;
	}

	while (consumed < available) {
		guint8 d = data[consumed];
		gint64 max;
		switch (parser->step) {

			case STEP_FIRST_R0:
				if (d == '\r')
					parser->step = STEP_FIRST_N0;
				else
					g_string_append_c(parser->buf, d);
				++ consumed;
				continue;

			case STEP_FIRST_N0:
				if (d == '\n') {
					if (!_manage_and_renew_command(parser, parser->buf))
						return _build_rc(HPRC_ERROR, "CMD parsing error");
					parser->step = STEP_SEP_R0;
					++ consumed;
					continue;
				}
				return _build_rc(HPRC_ERROR, "CMD parsing error");

			case STEP_SEP_R0:
				if (d == '\r')
					parser->step = STEP_SEP_N0;
				else {
					if (!_manage_and_renew_header(parser, parser->buf))
						return _build_rc(HPRC_ERROR, "HDR parsing error");
					g_string_append_c(parser->buf, d);
					parser->step = STEP_HEADERS_R0;
				}
				++ consumed;
				continue;

			case STEP_SEP_N0:
				if (d == '\n') {
					++ consumed;
					if (!_manage_and_renew_header(parser, parser->buf))
						return _build_rc(HPRC_ERROR, "HDR parsing error");
					parser->step = STEP_BODY_ASIS;
					if (parser->content_read >= parser->content_length)
						return _build_rc(HPRC_SUCCESS, NULL);
					continue;
				}
				return _build_rc(HPRC_ERROR, "SEP parsing error");

			case STEP_HEADERS_R0:
				if (d == '\r')
					parser->step = STEP_HEADERS_N0;
				else
					g_string_append_c(parser->buf, d);
				++ consumed;
				continue;

			case STEP_HEADERS_N0:
				if (d == '\n') {
					parser->step = STEP_SEP_R0;
					++ consumed;
					continue;
				}
				return _build_rc(HPRC_ERROR, "HDR parsing error");

			case STEP_BODY_ASIS:
				max = available - consumed;
				if (max > (parser->content_length - parser->content_read))
					max = parser->content_length - parser->content_read;
				if (parser->body_provider)
					parser->body_provider(data+consumed, max);
				consumed += max;
				parser->content_read += max;

				if (parser->content_read >= parser->content_length)
					return _build_rc(HPRC_SUCCESS, NULL);
				continue;
		}
	}

	return _build_rc(HPRC_MORE, NULL);
}

static void
http_parser_reset(struct http_parser_s *parser)
{
	parser->step = STEP_FIRST_R0;
	g_string_set_size(parser->buf, 0);
	parser->content_read = 0;
	parser->content_length = -1;
	if (parser->error)
		g_clear_error(&parser->error);
}

static struct http_parser_s*
http_parser_create(void)
{
	struct http_parser_s *parser = g_malloc0(sizeof(struct http_parser_s));
	parser->buf = g_string_sized_new(1024);
	http_parser_reset(parser);
	return parser;
}

static void
http_parser_destroy(struct http_parser_s *parser)
{
	if (!parser)
		return;
	g_string_free(parser->buf, TRUE);
	g_free(parser);
}

static gint
_cmp (gconstpointer p0, gconstpointer p1, gpointer p2)
{
	(void) p2;
	g_assert (p0 != NULL);
	g_assert (p1 != NULL);
	return g_ascii_strcasecmp(p0, p1);
}

static struct http_request_s *
http_request_create(struct network_client_s *client)
{
	struct http_request_s *req;
	req = g_malloc0(sizeof(*req));
	req->client = client;
	req->tree_headers = g_tree_new_full(_cmp, NULL, g_free, g_free);
	req->body = g_byte_array_new();
	return req;
}

static void
http_request_clean(struct http_request_s *req)
{
	if (req->cmd)
		g_free(req->cmd);
	if (req->req_uri)
		g_free(req->req_uri);
	if (req->version)
		g_free(req->version);
	if (req->tree_headers)
		g_tree_destroy(req->tree_headers);
	if (req->body)
		g_byte_array_free(req->body, TRUE);
	g_free(req);
}

//------------------------------------------------------------------------------

void
http_request_dispatcher_clean(struct http_request_dispatcher_s *d)
{
	if (!d)
		return ;
	if (d->requests)
		g_array_free(d->requests, TRUE);
	g_free(d);
}

struct http_request_dispatcher_s *
transport_http_build_dispatcher(gpointer u,
		const struct http_request_descr_s *descr)
{
	const struct http_request_descr_s *d;
	struct http_request_dispatcher_s *dispatcher;

	dispatcher = g_malloc0(sizeof(*dispatcher));
	dispatcher->u = u;
	dispatcher->requests = g_array_new(FALSE, FALSE,
			sizeof(struct http_request_handler_s));

	for (d=descr; d && d->name && d->handler ;d++) {
		struct http_request_handler_s h;
		h.handler = d->handler;
		g_array_append_vals(dispatcher->requests, &h, 1);
	}

	return dispatcher;
}

//------------------------------------------------------------------------------

static void
http_context_clean(struct transport_client_context_s *ctx)
{
	if (!ctx)
		return;
	if (ctx->parser)
		http_parser_destroy(ctx->parser);
	if (ctx->request)
		http_request_clean(ctx->request);
	g_free(ctx);
}

void
transport_http_factory0(struct http_request_dispatcher_s *dispatcher,
		struct network_client_s *client)
{
	struct network_transport_s *transport;
	struct transport_client_context_s *client_context;

	client_context = g_malloc0(sizeof(struct transport_client_context_s));
	client_context->dispatcher = dispatcher;
	client_context->parser = http_parser_create();
	client_context->request = http_request_create(client);

	transport = &(client->transport);
	transport->client_context = client_context;
	transport->clean_context = http_context_clean;
	transport->notify_input = http_notify_input;
	transport->notify_error = NULL;

	network_client_allow_input(client, TRUE);
}

//------------------------------------------------------------------------------

static gboolean
sender(gpointer k, gpointer v, gpointer u)
{
	g_string_append((GString*)u, (gchar*)k);
	g_string_append((GString*)u, ": ");
	g_string_append((GString*)u, (gchar*)v);
	g_string_append((GString*)u, "\r\n");
	return FALSE;
}

static void
_access_log(struct req_ctx_s *r, gint status, gsize out_len, const gchar *tail)
{
	const char *reqid = g_tree_lookup(r->request->tree_headers, PROXYD_HEADER_REQID);

	gint64 now = oio_ext_monotonic_time ();
	gint64 diff_total = now - r->tv_start;
	gint64 diff_handler = now - r->tv_parsed;

	GString *gstr = g_string_sized_new(256);

	g_string_append(gstr, r->client->local_name);
	g_string_append_c(gstr, ' ');
	g_string_append(gstr, r->client->peer_name);

	g_string_append_printf(gstr,
			" %s %d %"G_GINT64_FORMAT".%06"G_GINT64_FORMAT" %"G_GSIZE_FORMAT" %s %s %s t=%"G_GINT64_FORMAT".%06"G_GINT64_FORMAT,
			r->request->cmd,
			status,
			diff_total / G_TIME_SPAN_SECOND, diff_total % G_TIME_SPAN_SECOND,
			out_len,
			(r->uid && *r->uid) ? r->uid : "-",
			(reqid && *reqid) ? reqid : "-",
			r->request->req_uri,
			diff_handler / G_TIME_SPAN_SECOND, diff_handler % G_TIME_SPAN_SECOND);

	if (tail) {
		g_string_append_c (gstr, ' ');
		g_string_append (gstr, tail);
	}

	g_log("access", GRID_LOGLVL_INFO, "%s", gstr->str);
	g_string_free(gstr, TRUE);
}

static GError *
http_manage_request(struct req_ctx_s *r)
{
	gboolean finalized = 0;
	int code = HTTP_CODE_INTERNAL_ERROR;
	gchar *msg = NULL, *access = NULL;
	GTree *headers = NULL;
	const gchar *content_type = "octet/stream";

	struct {
		guint8 *data;
		gsize len;
	} body;

	void subject (const char *id) {
		oio_str_replace (&r->uid, id);
	}

	void cleanup(void) {
		oio_str_clean (&msg);
		oio_str_clean (&access);
		oio_str_clean ((gchar**)&body.data);
		body.len = 0;
		if (headers) {
			g_tree_destroy(headers);
			headers = NULL;
		}
	}

	void set_status(int c, const gchar *m) {
		EXTRA_ASSERT(m != NULL);
		code = c;
		oio_str_replace (&msg, m);
	}

	void set_content_type(const gchar *type) {
		content_type = type;
	}

	void add_header(const gchar *n, gchar *v) {
		EXTRA_ASSERT(!finalized);
		g_tree_replace(headers, (gpointer)g_strdup(n), v);
	}

	void add_header_gstr(const gchar *n, GString *v) {
		add_header(n, g_string_free(v, FALSE));
	}

	void set_body (guint8 *d, gsize l) {
		if (body.data)
			g_free(body.data);
		body.data = d;
		body.len = l;
	}

	void set_body_gstr(GString *gstr) {
		gsize len = gstr->len;
		guint8 *data = (guint8*) g_string_free(gstr, FALSE);
		set_body(data, len);
	}

	void set_body_gba(GByteArray *gba) {
		gsize len = gba->len;
		guint8 *data = g_byte_array_free(gba, FALSE);
		set_body(data, len);
	}

	void finalize(void) {
		EXTRA_ASSERT(!finalized);
		finalized = TRUE;

		GString *buf = g_string_sized_new(256);

		// Set the status line
		g_string_append_printf(buf, "%s %d %s\r\n", r->request->version, code, msg);
		g_string_append_printf(buf, "Server: metacd_http/%s\r\n", OIOSDS_API_VERSION);

		if (0 == g_ascii_strcasecmp("HTTP/1.1", r->request->version)) {
			// Manage the "Connection" header of http/1.1
			gchar *v = g_tree_lookup(r->request->tree_headers, "connection");
			if (v && 0 == g_ascii_strcasecmp("Keep-Alive", v)) {
				g_string_append(buf, "Connection: Keep-Alive\r\n");
				r->close_after_request = FALSE;
			}
			else {
				g_string_append(buf, "Connection: Close\r\n");
				r->close_after_request = TRUE;
			}
		}

		// Add body-related headers
		if (content_type)
			g_string_append_printf(buf, "Content-Type: %s\r\n", content_type);
		g_string_append_printf(buf, "Content-Length: %"G_GSIZE_FORMAT"\r\n", body.len);
		if (body.data && body.len)
			g_string_append(buf, "Transfer-Encoding: identity\r\n");

		// Add Custom headers
		g_tree_foreach(headers, sender, buf);

		// Finalize and send the headers
		g_string_append(buf, "\r\n");
		network_client_send_slab(r->client, data_slab_make_gstr(buf));

		// Now send the body
		if (body.data && body.len)
			network_client_send_slab(r->client,
					data_slab_make_buffer(body.data, body.len));

		_access_log(r, code, body.len, access);
		body.data = NULL;
		body.len = 0;
	}

	void access_tail (const char *fmt, ...) {
		va_list args;
		va_start(args, fmt);
		gchar *s = g_strdup_vprintf (fmt, args);
		va_end(args);
		oio_str_reuse (&access, s);
	}

	void final_error(int c_, const char *m_) {
		if (!finalized) {
			set_body(NULL, 0);
			set_status(c_, m_);
			finalize();
			cleanup();
		}
	}

	EXTRA_ASSERT(r->dispatcher != NULL);

	struct http_reply_ctx_s reply = {
		.set_status = set_status,
		.set_content_type = set_content_type,
		.add_header = add_header,
		.add_header_gstr = add_header_gstr,
		.set_body = set_body,
		.set_body_gba = set_body_gba,
		.set_body_gstr = set_body_gstr,
		.subject = subject,
		.finalize = finalize,
		.access_tail = access_tail,
	};

	finalized = FALSE;
	headers = g_tree_new_full(hashstr_quick_cmpdata, NULL, g_free, g_free);
	body.data = NULL;
	body.len = 0;

	if (NULL == r->request->req_uri || NULL == r->request->cmd) {
		final_error(HTTP_CODE_BAD_REQUEST, "Bad request");
		return NULL;
	}

	GArray *ga = r->dispatcher->requests;
	for (guint i=0; i < ga->len ;++i) {
		enum http_rc_e rc;
		struct http_request_handler_s *h;
		h = &g_array_index(ga, struct http_request_handler_s, i);
		EXTRA_ASSERT(h->handler != NULL);
		rc = h->handler(r->dispatcher->u, r->request, &reply);
		switch (rc) {
			case HTTPRC_DONE:
				EXTRA_ASSERT(finalized != FALSE);
				cleanup();
				return NULL;
			case HTTPRC_NEXT:
				break;
			case HTTPRC_ABORT:
				final_error(HTTP_CODE_INTERNAL_ERROR, "Internal error");
				return NEWERROR(HTTP_CODE_INTERNAL_ERROR, "HTTP handler error");
		}
	}

	EXTRA_ASSERT(!finalized);
	final_error(HTTP_CODE_NOT_FOUND, "No handler found");
	return NULL;
}

//------------------------------------------------------------------------------

static void
_lower(gchar *s)
{
	for (; *s ;++s)
		*s = g_ascii_tolower(*s);
}

static int
http_notify_input(struct network_client_s *clt)
{
	struct req_ctx_s r;

	void command_provider(const gchar *c, const gchar *s, const gchar *v) {
		r.request->cmd = g_strdup(c);
		r.request->req_uri = g_strdup(s);
		r.request->version = g_strdup(v);
	}
	void header_provider(const gchar *k0, const gchar *v) {
		gchar *k = g_strdup(k0);
		_lower(k);
		g_tree_replace(r.request->tree_headers, k, g_strdup(v));
	}
	void body_provider(const guint8 *data, gsize data_len) {
		g_byte_array_append(r.request->body, data, data_len);
	}

	memset(&r, 0, sizeof(r));
	r.close_after_request = TRUE;
	r.client = clt;
	r.transport = &(clt->transport);
	r.context = r.transport->client_context;
	r.dispatcher = r.context->dispatcher;
	r.request = r.context->request;

	struct http_parser_s *parser = clt->transport.client_context->parser;
	parser->command_provider = command_provider;
	parser->body_provider = body_provider;
	parser->header_provider = header_provider;

	gboolean done = FALSE;
	while (!done && data_slab_sequence_has_data(&clt->input)) {

		struct data_slab_s *slab;
		if (!(slab = data_slab_sequence_shift(&clt->input)))
			break;

		if (!data_slab_has_data(slab)) {
			data_slab_free(slab);
			continue;
		}

		guint8 *data = NULL;
		gsize data_size = (gsize)-1;

		if (!data_slab_consume(slab, &data, &data_size)) {
			data_slab_free(slab);
			continue;
		}

		if (!data || !data_size) {
			data_slab_free(slab);
			continue;
		}

		oio_str_clean (&r.uid);

		struct http_parsing_result_s rc = http_parse(parser, data, data_size);

		if (rc.status == HPRC_SUCCESS) {

			// Important times are now known.$
			// First, the last chunk of data received
			// Second, the moment the real treatment start ... i.e. now!
			r.tv_start = clt->time.evt_in;
			r.tv_parsed = oio_ext_monotonic_time ();

			GError *err = http_manage_request(&r);

			http_parser_reset(parser);
			http_request_clean(r.request);
			r.request = r.context->request = http_request_create(r.client);

			if (err) {
				GRID_INFO("Request management error : %d %s", err->code, err->message);
				g_clear_error(&err);
				network_client_allow_input(clt, FALSE);
				network_client_close_output(clt, 0);
				done = TRUE;
			}
			else if (r.close_after_request) {
				GRID_DEBUG("No connection keep-alive, closing.");
				network_client_allow_input(clt, FALSE);
				network_client_close_output(clt, 0);
				done = TRUE;
			}
		}
		else if (rc.status == HPRC_ERROR) {
			GRID_DEBUG("Request parsing error");
			network_client_allow_input(clt, FALSE);
			network_client_close_output(clt, 0);
			done = TRUE;
		}

		data_slab_sequence_unshift(&clt->input, slab);
	}

	parser->command_provider = NULL;
	parser->body_provider = NULL;
	parser->header_provider = NULL;
	oio_str_clean (&r.uid);
	return clt->transport.waiting_for_close ? RC_NODATA : RC_PROCESSED;
}

