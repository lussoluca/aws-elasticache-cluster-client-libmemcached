/**
 * Copyright (C) 2022 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#if defined(USE_TLS) && USE_TLS

#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/opensslv.h>

#include <libmemcached-1.0/tls.h>
#include <libmemcached/common.h>
#include <libmemcached/virtual_bucket.h>

//typedef enum {
//    WANT_READ = 1,
//    WANT_WRITE,
//    WANT_NONE
//} want_t;

/* A wrapper for the openssl's SSL_CTX object. */
struct memc_SSL_CTX {
    /* Associated OpenSSL SSL_CTX as created by memcached_create_and_set_ssl_context() */
    SSL_CTX *ctx;

    /* Requested hostname for verefication.
     * The hostname set needs to match with the CN (Common Name) of Subject of server certificate */
    char *hostname;
};

/* A wrapper for the openssl's SSL object. */
typedef struct memc_SSL {
    /* OpenSSL SSL object. */
    SSL *ssl;

    /* Store the default IO functions to switch back to in case TLS is disabled */
    void *default_io_funcs;
//
//    /* A flag to indicate whether the SSL layer requires the underlying
//     * socket to be readable/writable (possible before write/read)*/
//    want_t want;
} memc_SSL;

int memcached_init_OpenSSL(void)
{
    SSL_library_init();
#ifdef HIREDIS_USE_CRYPTO_LOCKS
    initOpensslLocks();
#endif

    return MEMCACHED_SUCCESS;
}

static context_funcs context_ssl_funcs = {
    .free_privctx = memcached_ssl_free,
    .read = memcached_ssl_read,
    .write = memcached_ssl_write
};

///* Update file events based on connection's need and the current status (mask)
// * The logic goes as:
// * 1.Conn needs read but mask not readable => create a readable event;
// * 2.Conn doesn't need read but musk readable => delete the event;
// * 3.Conn needs write but mask not writable => create a writable event;
// * 4.Conn doesn't need write but musk writable => delete the event. */
//static void update_ssl_event(memcached_instance_st *instance, int tls_flag) {
//    int mask = instance->events();
//    int need_read = tls_flag == TLS_CONN_FLAG_WRITE_WANT_READ;
//    int need_write = tls_flag == TLS_CONN_FLAG_READ_WANT_WRITE;
//
//    if (need_read && !(mask & POLLIN)) {
//        instance->events(POLLIN);
//    }
//    if (!need_read && (mask & POLLIN)) {
//        instance->delete_event(POLLIN);
//    }
//    if (need_write && !(mask & POLLOUT)) {
//        instance->events(POLLOUT);
//    }
//    if (!need_write && (mask & POLLOUT)) {
//        instance->delete_event(POLLOUT);
//    }
//}

void memcached_free_memc_ssl_ctx(memcached_st* memc){
    if (!memc) {
        return;
    }

    memcached_free_SSL_ctx((memc_SSL_CTX*)memc->ssl_ctx);
    memc->ssl_ctx = NULL;
}

void memcached_ssl_free(memcached_instance_st *instance){
    if (instance == NULL) {
        return;
    }

    memc_SSL *memc_ssl =(memc_SSL*)instance->privctx;
    if (memc_ssl == NULL) {
        return;
    }
    if (memc_ssl->ssl != NULL) {
        SSL_free(memc_ssl->ssl);
        memc_ssl->ssl = NULL;
    }
    instance->io_funcs = (context_funcs *)memc_ssl->default_io_funcs;
    libmemcached_free(NULL, memc_ssl);
    instance->privctx = NULL;
}

/* Callback for passing a keyfile password stored as an sds to OpenSSL */
static int ssl_set_password_callback(char *buf, int size, int rwflag, void *u) {
    UNUSED(rwflag);

    const char *pass = (const char *)u;
    size_t pass_len;

    if (!pass) return -1;
    pass_len = strlen(pass);
    if (pass_len > (size_t) size) return -1;
    memcpy(buf, pass, pass_len);

    return (int) pass_len;
}

/**
 * Initiate a new SSL context
 */
static SSL_CTX* init_ctx(void)
{
    const SSL_METHOD *method;
    SSL_CTX *ctx;

    OpenSSL_add_all_algorithms();  /* Load cryptos, et.al. */
    SSL_load_error_strings();   /* Bring in and register error messages */

#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    method = TLS_client_method();
#else
    method = TLSv1_2_client_method();
#endif

    ctx = SSL_CTX_new(method);   /* Create new context */
    if ( ctx == NULL )
    {
        ERR_print_errors_fp(stderr);
    }
    return ctx;
}

const char *memcached_ssl_context_get_error(memc_ssl_context_error error)
{
    switch (error) {
        case MEMCACHED_SSL_CTX_SUCCESS:
            return "No error. Created SSL_CTX successfully\n";
        case MEMCACHED_SSL_INVALID_ARGUMENTS:
            return "The function was called with invalid arguments\n";
        case MEMCACHED_SSL_MEMORY_ALLOCATION_FAILURE:
            return "Out of memory\n";
        case MEMCACHED_SSL_CTX_CREATE_FAILED:
            return "Failed to create OpenSSL SSL_CTX\n";
        case MEMCACHED_SSL_CTX_CERT_KEY_REQUIRED:
            return "Client cert and key must both be specified\n";
        case MEMCACHED_SSL_CTX_HOSTNAME_REQUIRED:
            return "Hostname must be specified for verification unless skip_hostname_verify or skip_cert_verify are set to true\n";
        case MEMCACHED_SSL_CTX_CA_CERT_LOAD_FAILED:
            return "Failed to load CA Certificate or CA Path\n";
        case MEMCACHED_SSL_CTX_CLIENT_CERT_LOAD_FAILED:
            return "Failed to load client certificate\n";
        case MEMCACHED_SSL_CTX_PRIVATE_KEY_LOAD_FAILED:
            return "Failed to load private key\n";
        case MEMCACHED_SSL_CTX_CIPHERS_LOAD_FAILED:
            return "Failed to configure ciphers\n";
        case MEMCACHED_SSL_CTX_PRIVATE_KEY_MISMATCH:
            return "Private key does not match the public certificate\n";
        default:
            return "Unknown error code\n";
    }
}

/**
 * SSL Connection initialization.
 */
static memcached_return_t init_ssl_connection(memcached_instance_st *server, memc_SSL_CTX *memc_ssl_ctx) {
    int rv;
    SSL *ssl;
    memc_SSL *memc_ssl;
    SSL_CTX *ssl_ctx;

    if (!memc_ssl_ctx || !memc_ssl_ctx->ctx) {
         return memcached_set_error(*server, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("Couldn't initialize SSL, memc_SSL_CTX or SSL_CTX is null"));
    }

    if (server->privctx) {
        return memcached_set_error(*server, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("The server was already associated with SSL context"));
    }

    memc_ssl = (memc_SSL*)calloc(1, sizeof(memc_SSL));
    if (memc_ssl == NULL) {
        return memcached_set_error(*server, MEMCACHED_MEMORY_ALLOCATION_FAILURE, MEMCACHED_AT, memcached_literal_param("Out of memory"));
    }

    // store the current IO functions
    memc_ssl->default_io_funcs = (void *)server->io_funcs;
    // set TLS IO functions
    server->io_funcs = &context_ssl_funcs;

    ssl_ctx = memc_ssl_ctx->ctx;
    ssl = SSL_new(ssl_ctx);
    if (!ssl) {
        rv = ERR_peek_error();
        goto error;
    }
    SSL_set_mode(ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    WATCHPOINT_ASSERT(server->fd != INVALID_SOCKET);
    SSL_set_fd(ssl, server->fd);
    SSL_set_connect_state(ssl);

    if (memc_ssl_ctx->hostname) {
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
        if (!SSL_set1_host(ssl, memc_ssl_ctx->hostname)) {
            rv = ERR_peek_error();
            goto error;
        }
#else
        X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
        /* Enable automatic hostname checks */
        X509_VERIFY_PARAM_set_hostflags(param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
        if (!X509_VERIFY_PARAM_set1_host(param, ssl_ctx->hostname, sizeof(ssl_ctx->hostname) - 1)) {
            rv = ERR_peek_error();
            goto error;
        }
#endif
    }

    memc_ssl->ssl = ssl;

    ERR_clear_error();

    rv = SSL_connect(ssl);
    if (rv == 1) {
        goto connection_success;
    }

    rv = SSL_get_error(ssl, rv);
    if ((server->root->flags.no_block) &&
        (rv == SSL_ERROR_WANT_READ || rv == SSL_ERROR_WANT_WRITE)) {
        goto connection_success;
    }

    libmemcached_free(NULL, memc_ssl);
    goto error;
    {
    connection_success:
        // Check if we need to set the server's file descriptor to non-blocking mode
        if (SOCK_NONBLOCK && (fcntl(server->fd, F_SETFL, SOCK_NONBLOCK) == -1)) {
            return memcached_set_error(*server, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("Could not switch to non-blocking.\n"));
        }

        server->privctx = memc_ssl;
        return MEMCACHED_SUCCESS;
    }
    {
    error:
        if (ssl) {
            SSL_free(ssl);
        }
        char err[512];
        if (rv == SSL_ERROR_SYSCALL) {
            snprintf(err,sizeof(err)-1,"%s",  strerror(errno));
        } else {
            unsigned long e = ERR_peek_last_error();
            const char * e_reason = ERR_reason_error_string(e);
            snprintf(err,sizeof(err)-1,"%s", e_reason);
        }
        return memcached_set_error(*server, MEMCACHED_TLS_CONNECTION_ERROR, MEMCACHED_AT, memcached_literal_param(err));
    }
}

memcached_return_t memcached_ssl_connect(memcached_instance_st *server)
{
    memcached_return_t rc;
    memc_SSL_CTX *memc_ssl_ctx;
    SSL_CTX *ssl_ctx;
    SSL *ssl;

    if (server == NULL)
    {
      return MEMCACHED_INVALID_ARGUMENTS;
    }

    /* We want to verify that init_ssl_connection() won't fail on this, as it will
     * not own the SSL object in that case and we'll end up leaking.
     */
    if (server->privctx != NULL) {
        rc = memcached_set_error(*(server->root), MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("SSL was already initated for this server"));
        goto error;
    }

    memc_ssl_ctx = (memc_SSL_CTX*)server->root->ssl_ctx;
    if (memc_ssl_ctx == NULL) {
        rc = memcached_set_error(*(server->root), MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("SSL context needs to be set with memcached_create_and_set_ssl_context()"));
        goto error;
    }

    ssl_ctx = memc_ssl_ctx->ctx;
    if (ssl_ctx == NULL) {
        rc = memcached_set_error(*(server->root), MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("SSL context needs to be set with memcached_create_and_set_ssl_context()"));
        goto error;
    }

    if (!memcached_is_no_block(server->root)) {
        // In blocking IO mode we want to enable auto retries after re-negotiations.
        // For more info see SSL_MODE_AUTO_RETRY in https://www.openssl.org/docs/man1.0.2/man3/SSL_CTX_set_mode.html
        SSL_CTX_set_mode(ssl_ctx, SSL_MODE_AUTO_RETRY);
    }

    rc = init_ssl_connection(server, memc_ssl_ctx);
    if (rc == MEMCACHED_SUCCESS) {
        server->state= MEMCACHED_SERVER_STATE_TLS_CONNECTED;
        return rc;
    } else {
        goto error;
    }

    error:
    server->reset_socket();
    return rc;
}


memc_ssl_context_error memcached_create_and_set_ssl_context(memcached_st *ptr, memcached_ssl_context_config *ctx_config) {
    memc_ssl_context_error rc;
    memc_SSL_CTX *memc_ssl_ctx;
    memc_SSL_CTX *ssl_ctx;

    if (ptr == NULL || ctx_config == NULL)
    {
        rc = MEMCACHED_SSL_INVALID_ARGUMENTS;
        goto error;
    }

    ssl_ctx = (memc_SSL_CTX *)libmemcached_calloc(ptr, 1, sizeof(memc_SSL_CTX));

    if (ssl_ctx == NULL) {
        rc = MEMCACHED_SSL_MEMORY_ALLOCATION_FAILURE;
        goto error;
    }

    ssl_ctx->ctx = init_ctx();

    if (!ssl_ctx->ctx) {
        rc = MEMCACHED_SSL_CTX_CREATE_FAILED;
        goto error;
    }

    SSL_CTX_set_options(ssl_ctx->ctx, SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3);

#ifdef SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS
    SSL_CTX_set_options(ssl_ctx->ctx, SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS);
#endif

#ifdef SSL_OP_NO_COMPRESSION
    SSL_CTX_set_options(ssl_ctx->ctx, SSL_OP_NO_COMPRESSION);
#endif

    SSL_CTX_set_mode(ssl_ctx->ctx, SSL_MODE_ENABLE_PARTIAL_WRITE|SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    if(ctx_config->hostname == NULL && !(ctx_config->skip_hostname_verify || ctx_config->skip_cert_verify)) {
        rc = MEMCACHED_SSL_CTX_HOSTNAME_REQUIRED;
        goto error;
    }

    if(ctx_config->hostname && !ctx_config->skip_hostname_verify) {
        ssl_ctx->hostname = strdup(ctx_config->hostname);
    }

    if (!ctx_config->skip_cert_verify) {
        SSL_CTX_set_verify(ssl_ctx->ctx, SSL_VERIFY_PEER, NULL);
    }

    SSL_CTX_set_default_passwd_cb(ssl_ctx->ctx, ssl_set_password_callback);
    SSL_CTX_set_default_passwd_cb_userdata(ssl_ctx->ctx, (void *) ctx_config->key_file_pass);

    /* load certificates */
    if (ctx_config->cert_file == NULL || ctx_config->key_file == NULL) {
        rc = MEMCACHED_SSL_CTX_CERT_KEY_REQUIRED;
        goto error;
    }

    if (ctx_config->ca_cert_dir || ctx_config->ca_cert_file) {
        if (SSL_CTX_load_verify_locations(ssl_ctx->ctx, ctx_config->ca_cert_file, ctx_config->ca_cert_dir) <= 0) {
            rc = MEMCACHED_SSL_CTX_CA_CERT_LOAD_FAILED;
            goto error;
        }
    }

    if (ctx_config->cert_file) {
        if (SSL_CTX_use_certificate_file(ssl_ctx->ctx, ctx_config->cert_file, SSL_FILETYPE_PEM) <= 0) {
            rc = MEMCACHED_SSL_CTX_CLIENT_CERT_LOAD_FAILED;
            goto error;
        }
        if (SSL_CTX_use_PrivateKey_file(ssl_ctx->ctx, ctx_config->key_file, SSL_FILETYPE_PEM) <= 0) {
            rc = MEMCACHED_SSL_CTX_PRIVATE_KEY_LOAD_FAILED;
            goto error;
        }
    }

    if (ctx_config->ciphers && !SSL_CTX_set_cipher_list(ssl_ctx->ctx, ctx_config->ciphers)) {
        rc = MEMCACHED_SSL_CTX_CIPHERS_LOAD_FAILED;
        goto error;
    }

#ifdef TLS1_3_VERSION
    if (ctx_config->ciphersuites && !SSL_CTX_set_ciphersuites(ssl_ctx->ctx, ctx_config->ciphersuites)) {
        rc = MEMCACHED_SSL_CTX_CIPHERSUITES_LOAD_FAILED;
        goto error;
    }
#endif

    ptr->ssl_ctx = ssl_ctx;

    return MEMCACHED_SSL_CTX_SUCCESS;

    error:
        if (ssl_ctx) {
            memcached_free_SSL_ctx(ssl_ctx);
        }
        return rc;
}

memc_SSL_CTX *memcached_get_ssl_context_copy(const memcached_st *ptr) {
    memc_SSL_CTX *src_ssl_ctx;
    memc_SSL_CTX *dst_ssl_ctx;
    if (ptr == NULL || ptr->ssl_ctx == NULL) {
        return NULL;
    }

    src_ssl_ctx = (memc_SSL_CTX*)ptr->ssl_ctx;

    if (src_ssl_ctx == NULL) {
        return NULL;
    }

    dst_ssl_ctx = (memc_SSL_CTX *)libmemcached_calloc(ptr, 1, sizeof(memc_SSL_CTX));
    dst_ssl_ctx->ctx = src_ssl_ctx->ctx;
    SSL_CTX_up_ref(src_ssl_ctx->ctx);

    if (src_ssl_ctx->hostname != NULL) {
        dst_ssl_ctx->hostname = strdup(src_ssl_ctx->hostname);
    }

    return dst_ssl_ctx;
}

memcached_return_t memcached_set_ssl_context(memcached_st *ptr, memc_SSL_CTX *ssl_ctx) {
  if (ptr == NULL || ssl_ctx == NULL || ssl_ctx->ctx == NULL)
  {
    return MEMCACHED_INVALID_ARGUMENTS;
  }
  ptr->ssl_ctx = ssl_ctx;
  return MEMCACHED_SUCCESS;
}

void memcached_free_SSL_ctx(memc_SSL_CTX *ssl_ctx)
{
    if (!ssl_ctx)
        return;

    if (ssl_ctx->hostname) {
        libmemcached_free(NULL, ssl_ctx->hostname);
        ssl_ctx->hostname = NULL;
    }

    if (ssl_ctx->ctx) {
        SSL_CTX_free(ssl_ctx->ctx);
        ssl_ctx->ctx = NULL;
    }

    libmemcached_free(NULL, ssl_ctx);
}

//static int handle_ssl_return_value(memcached_instance_st* instance, int rv, want_t *want){
//    if (rv <= 0) {
//        memc_SSL *memc_ssl = (memc_SSL*)instance->privctx;
//        int err = SSL_get_error(memc_ssl->ssl, rv);
//        switch (err) {
//            case SSL_ERROR_WANT_READ:
//                *want = WANT_READ;
//                return -1;
//            case SSL_ERROR_WANT_WRITE:
//                *want = WANT_WRITE;
//                return -1;
//            case SSL_ERROR_ZERO_RETURN:
//                // The TLS peer has closed the connection
//                memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT,
//                memcached_literal_param(ERR_reason_error_string(ERR_get_error())));
//                return 0;
//            default:
//                memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT,
//                                memcached_literal_param(ERR_reason_error_string(ERR_get_error())));
//                return -1;
//        }
//    }
//    return rv;
//}

static int handle_ssl_return_value(memcached_instance_st* instance, int rv){
    if (rv <= 0) {
        char err_buf[512];
        memc_SSL *memc_ssl = (memc_SSL*)instance->privctx;
        int err = SSL_get_error(memc_ssl->ssl, rv);
        switch (err) {
            case SSL_ERROR_WANT_READ:
                snprintf(err_buf,sizeof(err_buf)-1,"Got SSL_ERROR_WANT_READ: %s", ERR_reason_error_string(ERR_get_error()));
                memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param(err_buf));
                return -1;
            case SSL_ERROR_WANT_WRITE:
                snprintf(err_buf,sizeof(err_buf)-1,"Got SSL_ERROR_WANT_WRITE: %s", ERR_reason_error_string(ERR_get_error()));
                memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param(err_buf));
                return -1;
            case SSL_ERROR_ZERO_RETURN:
                // The TLS peer has closed the connection
                memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT,
                    memcached_literal_param(ERR_reason_error_string(ERR_get_error())));
                return 0;
            default:
                memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT,
                                memcached_literal_param(ERR_reason_error_string(ERR_get_error())));
                return -1;
        }
    }
    return rv;
}

ssize_t memcached_ssl_read(memcached_instance_st* instance,
             char* input_buf,
             size_t buffer_length,
             int flags){
    memc_SSL *memc_ssl = (memc_SSL*)instance->privctx;
    SSL *ssl = memc_ssl->ssl;

    int nread = SSL_read(ssl, input_buf, buffer_length);
    return handle_ssl_return_value(instance, nread);
}

ssize_t memcached_ssl_write(memcached_instance_st* instance,
             char* local_write_ptr,
             size_t write_length,
             int flags){
    memc_SSL *memc_ssl = (memc_SSL*)instance->privctx;
    SSL *ssl = memc_ssl->ssl;
    int nread = SSL_write(ssl, local_write_ptr, write_length);
    return handle_ssl_return_value(instance, nread);
//    do {
//        want_t want = WANT_NONE;
//        int nread = SSL_write(ssl, local_write_ptr, write_length);
//        rv = handle_ssl_return_value(instance, nread, &want);
//        if (rv > 0) {
//            // Write succeeded
//            break;
//        }
//        else if (rv <= 0 && want != WANT_NONE) {
//            // Re-negotation is taking place and WANT_READ/WANT_WRITE error was thrown,
//            // we should wait for the socket to be readable/writable before we can continue
//            memcached_return_t rc;
//            if (want == WANT_READ) {
//                rc = memcached_io_wait_for_read(instance);
//            } else {
//                rc = memcached_io_wait_for_write(instance);
//            }
//            if (memcached_success(rc)) {
//                continue;
//            } else {
//
//                break;
//            }
//        } else {
//            // Other error was thrown, bubble it down to be handled by the calling function
//            break;
//        }
//    } while(false);
//    return rv;
}

memcached_return_t memcached_ssl_get_server_certs(memcached_instance_st * instance, char ** output)
{
    SSL* ssl;
    X509 *cert;
    char *line;
    if (instance->privctx == NULL) {
        return memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("The instance doesn't have SSL context\n"));
    }
    ssl = ((memc_SSL *)instance->privctx)->ssl;
    if (ssl == NULL) {
        return memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("SSL connection wasn't yet established for this server\n"));
    }
    cert = SSL_get_peer_certificate(ssl); /* get the server's certificate */
    if ( cert != NULL )
    {
        char * certs_msg = (char *)libmemcached_malloc(instance->root, 100);
        snprintf(certs_msg,sizeof(certs_msg)-1,"Server certificates:\n Subject: %s\n Issuer: %s\n",
                 X509_NAME_oneline(X509_get_subject_name(cert), 0, 0),
                 X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0));
        *output = certs_msg;
    }
    else
        return memcached_set_error(*instance, MEMCACHED_TLS_ERROR, MEMCACHED_AT, memcached_literal_param("No certificates.\n"));
}

#endif //USE_TLS