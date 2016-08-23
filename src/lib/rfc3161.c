/*
 *    "This product includes software developed by the OpenSSL Project
 *     *    for use in the OpenSSL Toolkit (http://www.OpenSSL.org/)"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <civetweb.h>
#include <openssl/opensslconf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ts.h>
#include <openssl/bn.h>

/* Name of config entry that defines the OID file. */
#define ENV_OID_FILE "oid_file"

static ASN1_OBJECT *txt2obj(const char *oid);
static CONF *load_config_file(const char *configfile);

/* Reply related functions. */
static int reply_command(CONF *conf, char *section, char *engine,
                         char *queryfile, char *passin, char *inkey,
                         const EVP_MD *md, char *signer, char *chain,
                         const char *policy, char *in, int token_in, char *out,
                         int token_out, int text);
static TS_RESP *read_PKCS7(BIO *in_bio);
static TS_RESP *create_response(CONF *conf, const char *section, char *engine,
                                char *queryfile, char *passin, char *inkey,
                                const EVP_MD *md, char *signer, char *chain,
                                const char *policy);
static ASN1_INTEGER *serial_cb(TS_RESP_CTX *ctx, void *data);
static ASN1_INTEGER *next_serial(const char *serialfile);
static int save_ts_serial(const char *serialfile, ASN1_INTEGER *serial);

#define B_FORMAT_TEXT 0x8000
#define FORMAT_UNDEF 0
#define FORMAT_TEXT (1 | B_FORMAT_TEXT) /* Generic text */
#define FORMAT_ASN1 4                   /* ASN.1/DER */

/*
   int ts_http_respond(short event, ad_conn_t *conn, void *userdata) {
   if (event & AD_EVENT_READ) {
   if (ad_http_get_status(conn) == AD_HTTP_REQ_DONE) {
   ad_http_response(conn, 200, "text/html", "Hello World", 11);
   return ad_http_is_keepalive_request(conn) ? AD_DONE : AD_CLOSE;
   }
   }
   return AD_OK;
   }

   int http_default_handler(short event, ad_conn_t *conn, void *userdata) {
   if (event & AD_EVENT_READ) {
   if (ad_http_get_status(conn) == AD_HTTP_REQ_DONE) {
   ad_http_response(conn, 501, "text/html", "Not implemented", 15);
   return AD_CLOSE; // Close connection.
   }
   }
   return AD_OK;
   }
   */

// This function will be called by civetweb on every new request.
static int begin_request_handler(struct mg_connection *conn) {
    const struct mg_request_info *request_info = mg_get_request_info(conn);
    char content[100];

    // Prepare the message we're going to send
    int content_length = snprintf(content, sizeof(content),
                                  "Hello from civetweb! Remote port: %d",
                                  request_info->remote_port);

    // Send HTTP reply to the client
    mg_printf(conn,
              "HTTP/1.1 200 OK\r\n"
              "Content-Type: text/plain\r\n"
              "Content-Length: %d\r\n" // Always set Content-Length
              "\r\n"
              "%s",
              content_length, content);

    // Returning non-zero tells civetweb that our function has replied to
    // the client, and civetweb should not send client any more data.
    return 1;
}

int http_server_start() {
    struct mg_context *ctx;
    struct mg_callbacks callbacks;

    // List of options. Last element must be NULL.
    const char *options[] = {"listening_ports", "8080", NULL};

    // Prepare callbacks structure. We have only one callback, the rest are
    // NULL.
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.begin_request = begin_request_handler;

    // Start the web server.
    ctx = mg_start(&callbacks, NULL, options);

    // Wait until user hits "enter". Server is running in separate thread.
    // Navigating to http://localhost:8080 will invoke begin_request_handler().
    getchar();

    // Stop the server.
    mg_stop(ctx);

    return 0;
}

/*
 * Configuration file-related function definitions.
 */

static ASN1_OBJECT *txt2obj(const char *oid) {
    ASN1_OBJECT *oid_obj = NULL;

    if ((oid_obj = OBJ_txt2obj(oid, 0)) == NULL)
        //		BIO_printf(bio_err, "cannot convert %s to OID\n", oid);

        return oid_obj;
}

// static CONF *load_config_file(const char *configfile)
//{
//	CONF *conf = app_load_config(configfile);
//
//	if (conf != NULL) {
//		const char *p;
//
////		BIO_printf(bio_err, "Using configuration from %s\n",
///configfile);
//		p = NCONF_get_string(conf, NULL, ENV_OID_FILE);
//		if (p != NULL) {
//			BIO *oid_bio = BIO_new_file(p, "r");
//			if (!oid_bio)
//				ERR_print_errors(bio_err);
//			else {
//				OBJ_create_objects(oid_bio);
//				BIO_free_all(oid_bio);
//			}
//		} else
//			ERR_clear_error();
//		if (!add_oid_section(conf))
//			ERR_print_errors(bio_err);
//	}
//	return conf;
//}

/*
 * Reply-related method definitions.
 */

static int reply_command(CONF *conf, char *section, char *engine,
                         char *queryfile, char *passin, char *inkey,
                         const EVP_MD *md, char *signer, char *chain,
                         const char *policy, char *in, int token_in, char *out,
                         int token_out, int text) {
    int ret = 0;
    TS_RESP *response = NULL;
    BIO *in_bio = NULL;
    BIO *query_bio = NULL;
    BIO *inkey_bio = NULL;
    BIO *signer_bio = NULL;
    BIO *out_bio = NULL;
    BIO *bio_err;

    if (in != NULL) {
        if ((in_bio = BIO_new_file(in, "rb")) == NULL)
            goto end;
        if (token_in) {
            response = read_PKCS7(in_bio);
        } else {
            response = d2i_TS_RESP_bio(in_bio, NULL);
        }
    } else {
        response = create_response(conf, section, engine, queryfile, passin,
                                   inkey, md, signer, chain, policy);
        //		if (response)
        //			BIO_printf(bio_err, "Response has been
        //generated.\n");
        //		else
        //			BIO_printf(bio_err, "Response is not
        //generated.\n");
    }
    if (response == NULL)
        goto end;

    /* Write response. */
    if (text) {
        //		if ((out_bio = bio_open_default(out, 'w', FORMAT_TEXT)) ==
        //NULL)
        //			goto end;
        if (token_out) {
            TS_TST_INFO *tst_info = TS_RESP_get_tst_info(response);
            if (!TS_TST_INFO_print_bio(out_bio, tst_info))
                goto end;
        } else {
            if (!TS_RESP_print_bio(out_bio, response))
                goto end;
        }
    } else {
        //		if ((out_bio = bio_open_default(out, 'w', FORMAT_ASN1)) ==
        //NULL)
        //			goto end;
        if (token_out) {
            PKCS7 *token = TS_RESP_get_token(response);
            if (!i2d_PKCS7_bio(out_bio, token))
                goto end;
        } else {
            if (!i2d_TS_RESP_bio(out_bio, response))
                goto end;
        }
    }

    ret = 1;

end:
    ERR_print_errors(bio_err);
    BIO_free_all(in_bio);
    BIO_free_all(query_bio);
    BIO_free_all(inkey_bio);
    BIO_free_all(signer_bio);
    BIO_free_all(out_bio);
    TS_RESP_free(response);
    return ret;
}

/* Reads a PKCS7 token and adds default 'granted' status info to it. */
static TS_RESP *read_PKCS7(BIO *in_bio) {
    int ret = 0;
    PKCS7 *token = NULL;
    TS_TST_INFO *tst_info = NULL;
    TS_RESP *resp = NULL;
    TS_STATUS_INFO *si = NULL;

    if ((token = d2i_PKCS7_bio(in_bio, NULL)) == NULL)
        goto end;
    if ((tst_info = PKCS7_to_TS_TST_INFO(token)) == NULL)
        goto end;
    if ((resp = TS_RESP_new()) == NULL)
        goto end;
    if ((si = TS_STATUS_INFO_new()) == NULL)
        goto end;
    //	if (!TS_STATUS_INFO_set_status(si, TS_STATUS_GRANTED))
    //		goto end;
    if (!TS_RESP_set_status_info(resp, si))
        goto end;
    TS_RESP_set_tst_info(resp, token, tst_info);
    token = NULL;    /* Ownership is lost. */
    tst_info = NULL; /* Ownership is lost. */
    ret = 1;

end:
    PKCS7_free(token);
    TS_TST_INFO_free(tst_info);
    if (!ret) {
        TS_RESP_free(resp);
        resp = NULL;
    }
    TS_STATUS_INFO_free(si);
    return resp;
}

static TS_RESP *create_response(CONF *conf, const char *section, char *engine,
                                char *queryfile, char *passin, char *inkey,
                                const EVP_MD *md, char *signer, char *chain,
                                const char *policy) {
    int ret = 0;
    TS_RESP *response = NULL;
    BIO *query_bio = NULL;
    TS_RESP_CTX *resp_ctx = NULL;

    if ((query_bio = BIO_new_file(queryfile, "rb")) == NULL)
        goto end;
    if ((section = TS_CONF_get_tsa_section(conf, section)) == NULL)
        goto end;
    if ((resp_ctx = TS_RESP_CTX_new()) == NULL)
        goto end;
    if (!TS_CONF_set_serial(conf, section, serial_cb, resp_ctx))
        goto end;
#ifndef OPENSSL_NO_ENGINE
    if (!TS_CONF_set_crypto_device(conf, section, engine))
        goto end;
#endif
    if (!TS_CONF_set_signer_cert(conf, section, signer, resp_ctx))
        goto end;
    if (!TS_CONF_set_certs(conf, section, chain, resp_ctx))
        goto end;
    if (!TS_CONF_set_signer_key(conf, section, inkey, passin, resp_ctx))
        goto end;

    //	if (md) {
    //		if (!TS_RESP_CTX_set_signer_digest(resp_ctx, md))
    //			goto end;
    //	} else if (!TS_CONF_set_signer_digest(conf, section, NULL, resp_ctx)) {
    //		goto end;
    //	}

    if (!TS_CONF_set_def_policy(conf, section, policy, resp_ctx))
        goto end;
    if (!TS_CONF_set_policies(conf, section, resp_ctx))
        goto end;
    if (!TS_CONF_set_digests(conf, section, resp_ctx))
        goto end;
    if (!TS_CONF_set_accuracy(conf, section, resp_ctx))
        goto end;
    if (!TS_CONF_set_clock_precision_digits(conf, section, resp_ctx))
        goto end;
    if (!TS_CONF_set_ordering(conf, section, resp_ctx))
        goto end;
    if (!TS_CONF_set_tsa_name(conf, section, resp_ctx))
        goto end;
    if (!TS_CONF_set_ess_cert_id_chain(conf, section, resp_ctx))
        goto end;
    if ((response = TS_RESP_create_response(resp_ctx, query_bio)) == NULL)
        goto end;
    ret = 1;

end:
    if (!ret) {
        TS_RESP_free(response);
        response = NULL;
    }
    TS_RESP_CTX_free(resp_ctx);
    BIO_free_all(query_bio);
    return response;
}

static ASN1_INTEGER *serial_cb(TS_RESP_CTX *ctx, void *data) {
    const char *serial_file = (const char *)data;
    ASN1_INTEGER *serial = next_serial(serial_file);

    if (!serial) {
        TS_RESP_CTX_set_status_info(ctx, TS_STATUS_REJECTION,
                                    "Error during serial number "
                                    "generation.");
        TS_RESP_CTX_add_failure_info(ctx, TS_INFO_ADD_INFO_NOT_AVAILABLE);
    } else
        save_ts_serial(serial_file, serial);

    return serial;
}

static ASN1_INTEGER *next_serial(const char *serialfile) {
    int ret = 0;
    BIO *in = NULL;
    ASN1_INTEGER *serial = NULL;
    BIGNUM *bn = NULL;

    if ((serial = ASN1_INTEGER_new()) == NULL)
        goto err;

    if ((in = BIO_new_file(serialfile, "r")) == NULL) {
        ERR_clear_error();
        //		BIO_printf(bio_err, "Warning: could not open file %s for
        //"
        //				"reading, using serial number: 1\n",
        //serialfile);
        if (!ASN1_INTEGER_set(serial, 1))
            goto err;
    } else {
        char buf[1024];
        if (!a2i_ASN1_INTEGER(in, serial, buf, sizeof(buf))) {
            //			BIO_printf(bio_err, "unable to load number from
            //%s\n",
            //					serialfile);
            goto err;
        }
        if ((bn = ASN1_INTEGER_to_BN(serial, NULL)) == NULL)
            goto err;
        ASN1_INTEGER_free(serial);
        serial = NULL;
        if (!BN_add_word(bn, 1))
            goto err;
        if ((serial = BN_to_ASN1_INTEGER(bn, NULL)) == NULL)
            goto err;
    }
    ret = 1;

err:
    if (!ret) {
        ASN1_INTEGER_free(serial);
        serial = NULL;
    }
    BIO_free_all(in);
    BN_free(bn);
    return serial;
}

static int save_ts_serial(const char *serialfile, ASN1_INTEGER *serial) {
    int ret = 0;
    BIO *out = NULL;

    if ((out = BIO_new_file(serialfile, "w")) == NULL)
        goto err;
    if (i2a_ASN1_INTEGER(out, serial) <= 0)
        goto err;
    if (BIO_puts(out, "\n") <= 0)
        goto err;
    ret = 1;
err:
    if (!ret)
        //		BIO_printf(bio_err, "could not save serial number to
        //%s\n",
        //				serialfile);
        BIO_free_all(out);
    return ret;
}