/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#ifndef BYO_CRYPTO

#    include <aws/io/channel_bootstrap.h>
#    include <aws/io/event_loop.h>
#    include <aws/io/file_utils.h>
#    include <aws/io/host_resolver.h>
#    include <aws/io/logging.h>
#    include <aws/io/private/event_loop_impl.h>
#    include <aws/io/socket.h>
#    include <aws/io/tls_channel_handler.h>

#    include <aws/common/clock.h>
#    include <aws/common/condition_variable.h>
#    include <aws/common/thread.h>

#    include <aws/testing/aws_test_harness.h>

#    include <aws/common/string.h>
#    include <read_write_test_handler.h>
#    include <statistics_handler_test.h>

#    include <aws/io/private/pki_utils.h>
#    include <aws/io/private/tls_channel_handler_private.h>

/* badssl.com has occasional lags, make this timeout longer so we have a
 * higher chance of actually testing something. */
#    define BADSSL_TIMEOUT_MS 10000

bool s_is_badssl_being_flaky(const struct aws_string *host_name, int error_code) {
    if (strstr(aws_string_c_str(host_name), "badssl.com") != NULL) {
        if (error_code == AWS_IO_SOCKET_TIMEOUT || error_code == AWS_IO_TLS_NEGOTIATION_TIMEOUT) {
            fprintf(
                AWS_TESTING_REPORT_FD,
                "Warning: badssl.com is timing out right now. Maybe run the test again later?\n");
            return true;
        }
    }
    return false;
}

struct tls_test_args {
    struct aws_allocator *allocator;
    struct aws_mutex *mutex;
    struct aws_condition_variable *condition_variable;
    struct aws_tls_connection_options *tls_options;
    struct aws_channel *channel;
    struct aws_channel_handler *rw_handler;
    struct aws_channel_slot *rw_slot;
    struct aws_byte_buf negotiated_protocol;
    struct aws_byte_buf server_name;
    int last_error_code;

    uint32_t tls_levels_negotiated;
    uint32_t desired_tls_levels;

    bool listener_destroyed;
    bool error_invoked;
    bool expects_error;
    bool server;
    bool shutdown_finished;
    bool setup_callback_invoked;
    bool creation_callback_invoked;
};

/* common structure for tls options */
struct tls_opt_tester {
    struct aws_tls_ctx_options ctx_options;
    struct aws_tls_ctx *ctx;
    struct aws_tls_connection_options opt;
};

static int s_tls_server_opt_tester_init(
    struct aws_allocator *allocator,
    struct tls_opt_tester *tester,
    const char *cert_path,
    const char *pkey_path) {

#    ifdef __APPLE__
    struct aws_byte_cursor pwd_cur = aws_byte_cursor_from_c_str("1234");
    ASSERT_SUCCESS(
        aws_tls_ctx_options_init_server_pkcs12_from_path(&tester->ctx_options, allocator, "unittests.p12", &pwd_cur));
#    else
    ASSERT_SUCCESS(
        aws_tls_ctx_options_init_default_server_from_path(&tester->ctx_options, allocator, cert_path, pkey_path));
    ASSERT_SUCCESS(
        aws_tls_ctx_options_override_default_trust_store_from_path(&tester->ctx_options, NULL, "ca_root.crt"));
#    endif /* __APPLE__ */

    aws_tls_ctx_options_set_alpn_list(&tester->ctx_options, "h2;http/1.1");
    tester->ctx = aws_tls_server_ctx_new(allocator, &tester->ctx_options);
    ASSERT_NOT_NULL(tester->ctx);

    aws_tls_connection_options_init_from_ctx(&tester->opt, tester->ctx);
    return AWS_OP_SUCCESS;
}

static int s_tls_client_opt_tester_init(
    struct aws_allocator *allocator,
    struct tls_opt_tester *tester,
    struct aws_byte_cursor server_name) {

    aws_io_library_init(allocator);

    aws_tls_ctx_options_init_default_client(&tester->ctx_options, allocator);

#    ifdef __APPLE__
    ASSERT_SUCCESS(
        aws_tls_ctx_options_override_default_trust_store_from_path(&tester->ctx_options, NULL, "unittests.crt"));
#    else
    ASSERT_SUCCESS(
        aws_tls_ctx_options_override_default_trust_store_from_path(&tester->ctx_options, NULL, "ca_root.crt"));
#    endif /* __APPLE__ */

    tester->ctx = aws_tls_client_ctx_new(allocator, &tester->ctx_options);
    aws_tls_connection_options_init_from_ctx(&tester->opt, tester->ctx);
    aws_tls_connection_options_set_alpn_list(&tester->opt, allocator, "h2;http/1.1");

    aws_tls_connection_options_set_server_name(&tester->opt, allocator, &server_name);

    return AWS_OP_SUCCESS;
}

static int s_tls_opt_tester_clean_up(struct tls_opt_tester *tester) {
    aws_tls_connection_options_clean_up(&tester->opt);
    aws_tls_ctx_options_clean_up(&tester->ctx_options);
    aws_tls_ctx_release(tester->ctx);
    return AWS_OP_SUCCESS;
}

/* common structure for test */
struct tls_common_tester {
    struct aws_mutex mutex;
    struct aws_condition_variable condition_variable;
    struct aws_event_loop_group *el_group;
    struct aws_host_resolver *resolver;
    struct aws_atomic_var current_time_ns;
    struct aws_atomic_var stats_handler;
};

static struct tls_common_tester c_tester;

/* common structure for a tls local server */
struct tls_local_server_tester {
    struct aws_socket_options socket_options;
    struct tls_opt_tester server_tls_opt_tester;
    struct aws_socket_endpoint endpoint;
    struct aws_server_bootstrap *server_bootstrap;
    struct aws_socket *listener;
};

static int s_tls_test_arg_init(
    struct aws_allocator *allocator,
    struct tls_test_args *test_arg,
    bool server,
    struct tls_common_tester *tls_c_tester) {
    AWS_ZERO_STRUCT(*test_arg);
    test_arg->mutex = &tls_c_tester->mutex;
    test_arg->condition_variable = &tls_c_tester->condition_variable;
    test_arg->allocator = allocator;
    test_arg->server = server;
    test_arg->desired_tls_levels = 1;

    return AWS_OP_SUCCESS;
}

static int s_tls_common_tester_init(struct aws_allocator *allocator, struct tls_common_tester *tester) {
    aws_io_library_init(allocator);
    AWS_ZERO_STRUCT(*tester);

    struct aws_mutex mutex = AWS_MUTEX_INIT;
    struct aws_condition_variable condition_variable = AWS_CONDITION_VARIABLE_INIT;
    tester->mutex = mutex;
    tester->condition_variable = condition_variable;
    aws_atomic_store_int(&tester->current_time_ns, 0);
    aws_atomic_store_ptr(&tester->stats_handler, NULL);

    struct aws_event_loop_group_options elg_options = {
        .loop_count = 0,
    };
    tester->el_group = aws_event_loop_group_new(allocator, &elg_options);

    struct aws_host_resolver_default_options resolver_options = {
        .el_group = tester->el_group,
        .max_entries = 1,
    };
    tester->resolver = aws_host_resolver_new_default(allocator, &resolver_options);

    return AWS_OP_SUCCESS;
}

static int s_tls_common_tester_clean_up(struct tls_common_tester *tester) {
    aws_host_resolver_release(tester->resolver);
    aws_event_loop_group_release(tester->el_group);

    aws_io_library_clean_up();

    aws_condition_variable_clean_up(&tester->condition_variable);
    aws_mutex_clean_up(&tester->mutex);
    return AWS_OP_SUCCESS;
}

static bool s_tls_channel_shutdown_predicate(void *user_data) {
    struct tls_test_args *setup_test_args = user_data;
    return setup_test_args->shutdown_finished || setup_test_args->last_error_code == AWS_IO_SOCKET_TIMEOUT ||
           (setup_test_args->expects_error && setup_test_args->error_invoked);
}

static bool s_tls_listener_destroy_predicate(void *user_data) {
    struct tls_test_args *setup_test_args = user_data;
    return setup_test_args->listener_destroyed || setup_test_args->last_error_code == AWS_IO_SOCKET_TIMEOUT;
}

static bool s_tls_channel_setup_predicate(void *user_data) {
    struct tls_test_args *setup_test_args = user_data;
    return (setup_test_args->tls_levels_negotiated == setup_test_args->desired_tls_levels &&
            setup_test_args->setup_callback_invoked) ||
           setup_test_args->error_invoked;
}

/*
 * test args mutex must be held before calling this function
 */
static void s_aws_check_for_user_handler_setup(struct tls_test_args *setup_test_args) {
    if (setup_test_args->tls_levels_negotiated == setup_test_args->desired_tls_levels &&
        setup_test_args->setup_callback_invoked) {
        if (setup_test_args->rw_handler) {
            struct aws_channel *channel = setup_test_args->channel;
            struct aws_channel_slot *rw_slot = aws_channel_slot_new(channel);
            aws_channel_slot_insert_end(channel, rw_slot);
            aws_channel_slot_set_handler(rw_slot, setup_test_args->rw_handler);
            setup_test_args->rw_slot = rw_slot;
        }
    }
}

static int s_add_tls_handler_to_end_of_channel(struct tls_test_args *setup_test_args) {
    AWS_FATAL_ASSERT(setup_test_args->desired_tls_levels > 1);
    AWS_FATAL_ASSERT(!setup_test_args->server);

    struct aws_channel_slot *last_slot = aws_channel_get_first_slot(setup_test_args->channel);
    while (last_slot->adj_right) {
        last_slot = last_slot->adj_right;
    }

    return aws_channel_setup_client_tls(last_slot, setup_test_args->tls_options);
}

static int s_on_channel_setup_next_tls_handler(struct tls_test_args *setup_test_args) {
    if (setup_test_args->tls_levels_negotiated < setup_test_args->desired_tls_levels) {
        ASSERT_SUCCESS(s_add_tls_handler_to_end_of_channel(setup_test_args));
    }

    return AWS_OP_SUCCESS;
}

static int s_on_tls_negotiated_next_tls_handler(struct tls_test_args *setup_test_args) {
    if (!setup_test_args->setup_callback_invoked) {
        return AWS_OP_SUCCESS;
    }

    if (setup_test_args->tls_levels_negotiated < setup_test_args->desired_tls_levels) {
        ASSERT_SUCCESS(s_add_tls_handler_to_end_of_channel(setup_test_args));
    }

    return AWS_OP_SUCCESS;
}

static void s_tls_handler_test_client_setup_callback(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;

    struct tls_test_args *setup_test_args = user_data;
    aws_mutex_lock(setup_test_args->mutex);

    setup_test_args->setup_callback_invoked = true;

    if (!error_code) {
        setup_test_args->channel = channel;
        s_aws_check_for_user_handler_setup(setup_test_args);
        s_on_channel_setup_next_tls_handler(setup_test_args);
    } else {
        setup_test_args->error_invoked = true;
        setup_test_args->last_error_code = error_code;
    }

    aws_mutex_unlock(setup_test_args->mutex);
    aws_condition_variable_notify_one(setup_test_args->condition_variable);
}

static void s_tls_handler_test_server_setup_callback(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;

    struct tls_test_args *setup_test_args = (struct tls_test_args *)user_data;

    aws_mutex_lock(setup_test_args->mutex);
    setup_test_args->setup_callback_invoked = true;
    if (!error_code) {
        setup_test_args->channel = channel;
    } else {
        setup_test_args->error_invoked = true;
        setup_test_args->last_error_code = error_code;
    }

    s_aws_check_for_user_handler_setup(setup_test_args);

    aws_mutex_unlock(setup_test_args->mutex);
    aws_condition_variable_notify_one(setup_test_args->condition_variable);
}

static void s_tls_handler_test_client_shutdown_callback(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    (void)error_code;
    (void)channel;

    struct tls_test_args *setup_test_args = (struct tls_test_args *)user_data;

    aws_mutex_lock(setup_test_args->mutex);
    setup_test_args->shutdown_finished = true;
    if (error_code) {
        setup_test_args->last_error_code = error_code;
    }
    aws_mutex_unlock(setup_test_args->mutex);
    aws_condition_variable_notify_one(setup_test_args->condition_variable);
}

static void s_tls_handler_test_server_shutdown_callback(
    struct aws_server_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    (void)error_code;
    (void)channel;

    struct tls_test_args *setup_test_args = (struct tls_test_args *)user_data;

    aws_mutex_lock(setup_test_args->mutex);
    setup_test_args->shutdown_finished = true;
    if (error_code) {
        setup_test_args->last_error_code = error_code;
    }
    aws_condition_variable_notify_one(setup_test_args->condition_variable);
    aws_mutex_unlock(setup_test_args->mutex);
}

static void s_tls_handler_test_server_listener_destroy_callback(
    struct aws_server_bootstrap *bootstrap,
    void *user_data) {
    (void)bootstrap;

    struct tls_test_args *setup_test_args = (struct tls_test_args *)user_data;
    aws_mutex_lock(setup_test_args->mutex);
    setup_test_args->listener_destroyed = true;
    aws_condition_variable_notify_all(setup_test_args->condition_variable);
    aws_mutex_unlock(setup_test_args->mutex);
}

static void s_tls_on_negotiated(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int err_code,
    void *user_data) {

    (void)slot;
    struct tls_test_args *setup_test_args = (struct tls_test_args *)user_data;

    if (!err_code) {
        aws_mutex_lock(setup_test_args->mutex);

        if (aws_tls_is_alpn_available()) {
            setup_test_args->negotiated_protocol = aws_tls_handler_protocol(handler);
        }
        setup_test_args->server_name = aws_tls_handler_server_name(handler);
        ++setup_test_args->tls_levels_negotiated;

        s_aws_check_for_user_handler_setup(setup_test_args);
        s_on_tls_negotiated_next_tls_handler(setup_test_args);

        aws_mutex_unlock(setup_test_args->mutex);
    }

    aws_condition_variable_notify_one(setup_test_args->condition_variable);
}

static int s_tls_local_server_tester_init(
    struct aws_allocator *allocator,
    struct tls_local_server_tester *tester,
    struct tls_test_args *args,
    struct tls_common_tester *tls_c_tester,
    bool enable_back_pressure,
    const char *cert_path,
    const char *pkey_path) {
    AWS_ZERO_STRUCT(*tester);
    ASSERT_SUCCESS(s_tls_server_opt_tester_init(allocator, &tester->server_tls_opt_tester, cert_path, pkey_path));
    aws_tls_connection_options_set_callbacks(&tester->server_tls_opt_tester.opt, s_tls_on_negotiated, NULL, NULL, args);
    tester->socket_options.connect_timeout_ms = 3000;
    tester->socket_options.type = AWS_SOCKET_STREAM;
    tester->socket_options.domain = AWS_SOCKET_LOCAL;

    aws_socket_endpoint_init_local_address_for_test(&tester->endpoint);

    tester->server_bootstrap = aws_server_bootstrap_new(allocator, tls_c_tester->el_group);
    ASSERT_NOT_NULL(tester->server_bootstrap);

    struct aws_server_socket_channel_bootstrap_options bootstrap_options = {
        .bootstrap = tester->server_bootstrap,
        .enable_read_back_pressure = enable_back_pressure,
        .port = tester->endpoint.port,
        .host_name = tester->endpoint.address,
        .socket_options = &tester->socket_options,
        .incoming_callback = s_tls_handler_test_server_setup_callback,
        .shutdown_callback = s_tls_handler_test_server_shutdown_callback,
        .destroy_callback = s_tls_handler_test_server_listener_destroy_callback,
        .tls_options = &tester->server_tls_opt_tester.opt,
        .user_data = args,
    };
    tester->listener = aws_server_bootstrap_new_socket_listener(&bootstrap_options);
    ASSERT_NOT_NULL(tester->listener);

    return AWS_OP_SUCCESS;
}

static int s_tls_local_server_tester_clean_up(struct tls_local_server_tester *tester) {
    ASSERT_SUCCESS(s_tls_opt_tester_clean_up(&tester->server_tls_opt_tester));
    aws_server_bootstrap_release(tester->server_bootstrap);
    return AWS_OP_SUCCESS;
}

struct tls_test_rw_args {
    struct aws_mutex *mutex;
    struct aws_condition_variable *condition_variable;
    struct aws_byte_buf received_message;
    int read_invocations;
    bool invocation_happened;
};

static int s_tls_rw_args_init(
    struct tls_test_rw_args *args,
    struct tls_common_tester *tls_c_tester,
    struct aws_byte_buf received_message) {
    AWS_ZERO_STRUCT(*args);
    args->mutex = &tls_c_tester->mutex;
    args->condition_variable = &tls_c_tester->condition_variable;
    args->received_message = received_message;
    return AWS_OP_SUCCESS;
}

static bool s_tls_test_read_predicate(void *user_data) {
    struct tls_test_rw_args *rw_args = (struct tls_test_rw_args *)user_data;

    return rw_args->invocation_happened;
}

static struct aws_byte_buf s_tls_test_handle_read(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_byte_buf *data_read,
    void *user_data) {

    (void)handler;
    (void)slot;

    struct tls_test_rw_args *rw_args = (struct tls_test_rw_args *)user_data;
    aws_mutex_lock(rw_args->mutex);

    aws_byte_buf_write_from_whole_buffer(&rw_args->received_message, *data_read);
    rw_args->read_invocations += 1;
    rw_args->invocation_happened = true;

    aws_mutex_unlock(rw_args->mutex);
    aws_condition_variable_notify_one(rw_args->condition_variable);

    return rw_args->received_message;
}

static struct aws_byte_buf s_tls_test_handle_write(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_byte_buf *data_read,
    void *user_data) {

    (void)handler;
    (void)slot;
    (void)data_read;
    (void)user_data;

    /*do nothing*/
    return (struct aws_byte_buf){0};
}

static uint8_t s_server_received_message[128] = {0};
static uint8_t s_client_received_message[128] = {0};

/* common structure for test with self-initaizlied server and client */
struct tls_channel_server_client_tester {
    struct tls_test_rw_args client_rw_args;
    struct tls_test_rw_args server_rw_args;
    struct tls_test_args client_args;
    struct tls_test_args server_args;
    struct aws_client_bootstrap *client_bootstrap;
    struct tls_local_server_tester local_server_tester;

    struct aws_mutex server_mutex;
    struct aws_condition_variable server_condition_variable;

    struct aws_atomic_var server_shutdown_invoked;
    /* Make sure server and client doesn't use the same thread */
    struct aws_event_loop_group *client_el_group;

    bool window_update_after_shutdown;
};

static struct tls_channel_server_client_tester s_server_client_tester;

static int s_tls_channel_server_client_tester_init(struct aws_allocator *allocator) {
    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));
    AWS_ZERO_STRUCT(s_server_client_tester);
    ASSERT_SUCCESS(aws_mutex_init(&s_server_client_tester.server_mutex));
    ASSERT_SUCCESS(aws_condition_variable_init(&s_server_client_tester.server_condition_variable));

    struct aws_event_loop_group_options elg_options = {
        .loop_count = 0,
    };
    s_server_client_tester.client_el_group = aws_event_loop_group_new(allocator, &elg_options);

    ASSERT_SUCCESS(s_tls_rw_args_init(
        &s_server_client_tester.server_rw_args,
        &c_tester,
        aws_byte_buf_from_empty_array(s_server_received_message, sizeof(s_server_received_message))));
    s_server_client_tester.server_rw_args.mutex = &s_server_client_tester.server_mutex;
    s_server_client_tester.server_rw_args.condition_variable = &s_server_client_tester.server_condition_variable;
    ASSERT_SUCCESS(s_tls_rw_args_init(
        &s_server_client_tester.client_rw_args,
        &c_tester,
        aws_byte_buf_from_empty_array(s_client_received_message, sizeof(s_client_received_message))));
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &s_server_client_tester.client_args, false, &c_tester));
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &s_server_client_tester.server_args, true, &c_tester));
    s_server_client_tester.server_args.mutex = &s_server_client_tester.server_mutex;
    s_server_client_tester.server_args.condition_variable = &s_server_client_tester.server_condition_variable;

    ASSERT_SUCCESS(s_tls_local_server_tester_init(
        allocator,
        &s_server_client_tester.local_server_tester,
        &s_server_client_tester.server_args,
        &c_tester,
        true,
        "server.crt",
        "server.key"));
    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = s_server_client_tester.client_el_group,
        .host_resolver = c_tester.resolver,
    };
    s_server_client_tester.client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);

    aws_atomic_store_int(&s_server_client_tester.server_shutdown_invoked, 0);
    return AWS_OP_SUCCESS;
}

static int s_tls_channel_server_client_tester_cleanup(void) {
    /* Make sure client and server all shutdown */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable,
        &c_tester.mutex,
        s_tls_channel_shutdown_predicate,
        &s_server_client_tester.client_args));
    aws_mutex_unlock(&c_tester.mutex);

    aws_server_bootstrap_destroy_socket_listener(
        s_server_client_tester.local_server_tester.server_bootstrap,
        s_server_client_tester.local_server_tester.listener);
    ASSERT_SUCCESS(s_tls_local_server_tester_clean_up(&s_server_client_tester.local_server_tester));
    ASSERT_SUCCESS(aws_mutex_lock(&s_server_client_tester.server_mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &s_server_client_tester.server_condition_variable,
        &s_server_client_tester.server_mutex,
        s_tls_listener_destroy_predicate,
        &s_server_client_tester.server_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&s_server_client_tester.server_mutex));

    /* Clean up */
    aws_mutex_clean_up(&s_server_client_tester.server_mutex);
    aws_condition_variable_clean_up(&s_server_client_tester.server_condition_variable);
    aws_client_bootstrap_release(s_server_client_tester.client_bootstrap);
    aws_event_loop_group_release(s_server_client_tester.client_el_group);
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));
    return AWS_OP_SUCCESS;
}

static int s_set_socket_channel(struct tls_channel_server_client_tester *server_client_tester) {

    struct tls_opt_tester client_tls_opt_tester;
    struct aws_byte_cursor server_name = aws_byte_cursor_from_c_str("localhost");
    ASSERT_SUCCESS(
        s_tls_client_opt_tester_init(server_client_tester->client_args.allocator, &client_tls_opt_tester, server_name));
    aws_tls_connection_options_set_callbacks(
        &client_tls_opt_tester.opt, s_tls_on_negotiated, NULL, NULL, &server_client_tester->client_args);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = server_client_tester->client_bootstrap;
    channel_options.host_name = server_client_tester->local_server_tester.endpoint.address;
    channel_options.port = 0;
    channel_options.socket_options = &server_client_tester->local_server_tester.socket_options;
    channel_options.tls_options = &client_tls_opt_tester.opt;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &server_client_tester->client_args;
    channel_options.enable_read_back_pressure = true;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* put this here to verify ownership semantics are correct. This should NOT cause a segfault. If it does, ya
     * done messed up. */
    aws_tls_connection_options_clean_up(&client_tls_opt_tester.opt);
    /* wait for both ends to setup */
    ASSERT_SUCCESS(aws_mutex_lock(&s_server_client_tester.server_mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &s_server_client_tester.server_condition_variable,
        &s_server_client_tester.server_mutex,
        s_tls_channel_setup_predicate,
        &server_client_tester->server_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&s_server_client_tester.server_mutex));
    ASSERT_FALSE(server_client_tester->server_args.error_invoked);

/* currently it seems ALPN doesn't work in server mode. Just leaving this check out for now. */
#    ifndef __APPLE__
    struct aws_byte_buf expected_protocol = aws_byte_buf_from_c_str("h2");

    /* check ALPN and SNI was properly negotiated */
    if (aws_tls_is_alpn_available()) {
        ASSERT_BIN_ARRAYS_EQUALS(
            expected_protocol.buffer,
            expected_protocol.len,
            server_client_tester->server_args.negotiated_protocol.buffer,
            server_client_tester->server_args.negotiated_protocol.len);
    }
#    endif

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable,
        &c_tester.mutex,
        s_tls_channel_setup_predicate,
        &server_client_tester->client_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));
    ASSERT_FALSE(server_client_tester->client_args.error_invoked);

/* currently it seems ALPN doesn't work in server mode. Just leaving this check out for now. */
#    ifndef __MACH__
    if (aws_tls_is_alpn_available()) {
        ASSERT_BIN_ARRAYS_EQUALS(
            expected_protocol.buffer,
            expected_protocol.len,
            server_client_tester->client_args.negotiated_protocol.buffer,
            server_client_tester->client_args.negotiated_protocol.len);
    }
#    endif

    ASSERT_SUCCESS(s_tls_opt_tester_clean_up(&client_tls_opt_tester));
    return AWS_OP_SUCCESS;
}

static int s_tls_channel_echo_and_backpressure_test_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    ASSERT_SUCCESS(s_tls_channel_server_client_tester_init(allocator));
    struct tls_test_rw_args *client_rw_args = &s_server_client_tester.client_rw_args;
    struct tls_test_rw_args *server_rw_args = &s_server_client_tester.server_rw_args;
    struct tls_test_args *client_args = &s_server_client_tester.client_args;
    struct tls_test_args *server_args = &s_server_client_tester.server_args;

    struct aws_byte_buf read_tag = aws_byte_buf_from_c_str("I'm a little teapot.");
    struct aws_byte_buf write_tag = aws_byte_buf_from_c_str("I'm a big teapot");

    /* make the windows small to make sure back pressure is honored. */
    struct aws_channel_handler *client_rw_handler = rw_handler_new(
        allocator, s_tls_test_handle_read, s_tls_test_handle_write, true, write_tag.len / 2, client_rw_args);
    ASSERT_NOT_NULL(client_rw_handler);
    struct aws_channel_handler *server_rw_handler = rw_handler_new(
        allocator, s_tls_test_handle_read, s_tls_test_handle_write, true, read_tag.len / 2, server_rw_args);
    ASSERT_NOT_NULL(server_rw_handler);
    server_args->rw_handler = server_rw_handler;
    client_args->rw_handler = client_rw_handler;

    g_aws_channel_max_fragment_size = 4096;
    ASSERT_SUCCESS(s_set_socket_channel(&s_server_client_tester));

    /* Do the IO operations */
    rw_handler_write(client_args->rw_handler, client_args->rw_slot, &write_tag);
    rw_handler_write(server_args->rw_handler, server_args->rw_slot, &read_tag);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_test_read_predicate, client_rw_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    ASSERT_SUCCESS(aws_mutex_lock(&s_server_client_tester.server_mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &s_server_client_tester.server_condition_variable,
        &s_server_client_tester.server_mutex,
        s_tls_test_read_predicate,
        server_rw_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&s_server_client_tester.server_mutex));

    server_rw_args->invocation_happened = false;
    client_rw_args->invocation_happened = false;

    ASSERT_INT_EQUALS(1, client_rw_args->read_invocations);
    ASSERT_INT_EQUALS(1, server_rw_args->read_invocations);

    /* Go ahead and verify back-pressure works*/
    rw_handler_trigger_increment_read_window(server_args->rw_handler, server_args->rw_slot, 100);
    rw_handler_trigger_increment_read_window(client_args->rw_handler, client_args->rw_slot, 100);

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_test_read_predicate, client_rw_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    ASSERT_SUCCESS(aws_mutex_lock(&s_server_client_tester.server_mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &s_server_client_tester.server_condition_variable,
        &s_server_client_tester.server_mutex,
        s_tls_test_read_predicate,
        server_rw_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&s_server_client_tester.server_mutex));

    ASSERT_INT_EQUALS(2, client_rw_args->read_invocations);
    ASSERT_INT_EQUALS(2, server_rw_args->read_invocations);

    ASSERT_BIN_ARRAYS_EQUALS(
        write_tag.buffer, write_tag.len, server_rw_args->received_message.buffer, server_rw_args->received_message.len);
    ASSERT_BIN_ARRAYS_EQUALS(
        read_tag.buffer, read_tag.len, client_rw_args->received_message.buffer, client_rw_args->received_message.len);

    aws_channel_shutdown(server_args->channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_mutex_lock(&s_server_client_tester.server_mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &s_server_client_tester.server_condition_variable,
        &s_server_client_tester.server_mutex,
        s_tls_channel_shutdown_predicate,
        &s_server_client_tester.server_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&s_server_client_tester.server_mutex));

    /*no shutdown on the client necessary here (it should have been triggered by shutting down the other side). just
     * wait for the event to fire. */
    ASSERT_SUCCESS(s_tls_channel_server_client_tester_cleanup());

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(tls_channel_echo_and_backpressure_test, s_tls_channel_echo_and_backpressure_test_fn)

static struct aws_byte_buf s_on_client_recive_shutdown_with_cache_data(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_byte_buf *data_read,
    void *user_data) {

    /**
     * Client received the data from server, and it happens from the channel thread.
     * Because of the limited window size, we also have more data cached in the TLS hanlder.
     *
     * Now:
     * - Shutdown the server channel, and wait for it to finish, which will close the socket, and the socket will
     * schedule the channel shutdown process when this function returns.
     * - Update the window from this thread, it should schedule another task from channel thread to do so.
     */
    (void)slot;
    (void)user_data;
    struct tls_test_rw_args *client_rw_args = &s_server_client_tester.client_rw_args;

    if (!rw_handler_shutdown_called(handler)) {

        size_t shutdown_invoked = aws_atomic_load_int(&s_server_client_tester.server_shutdown_invoked);
        if (shutdown_invoked == 0) {
            aws_atomic_store_int(&s_server_client_tester.server_shutdown_invoked, 1);
            if (!s_server_client_tester.window_update_after_shutdown) {
                rw_handler_trigger_increment_read_window(
                    s_server_client_tester.client_args.rw_handler, s_server_client_tester.client_args.rw_slot, 100);
            }
            aws_channel_shutdown(s_server_client_tester.server_args.channel, AWS_OP_SUCCESS);

            aws_mutex_lock(&s_server_client_tester.server_mutex);
            aws_condition_variable_wait_pred(
                &s_server_client_tester.server_condition_variable,
                &s_server_client_tester.server_mutex,
                s_tls_channel_shutdown_predicate,
                &s_server_client_tester.server_args);
            aws_mutex_unlock(&s_server_client_tester.server_mutex);
        }
        aws_mutex_lock(client_rw_args->mutex);

        aws_byte_buf_write_from_whole_buffer(&client_rw_args->received_message, *data_read);
        client_rw_args->read_invocations += 1;
        client_rw_args->invocation_happened = true;

        aws_mutex_unlock(client_rw_args->mutex);
        aws_condition_variable_notify_one(client_rw_args->condition_variable);
    } else {
        AWS_FATAL_ASSERT(false && "The channel has already shutdown before process the message.");
    }
    return client_rw_args->received_message;
}

/**
 * Test that when the socket initailize the shutdown process becasue of socket closed, we have a pending window update
 * task to start the reading of the cached data in TLS handler. So, the channel will run the window update task and
 * followed by a shutdown task immediately.
 *
 * Previously, the window update task will schedule read task if it opens the window back from close, but since the
 * shutdown task already been scheluded, the read will happen after shutdown. So, it result in lost of data.
 */
static int s_tls_channel_shutdown_with_cache_test_helper(struct aws_allocator *allocator, bool after_shutdown) {
    ASSERT_SUCCESS(s_tls_channel_server_client_tester_init(allocator));
    s_server_client_tester.window_update_after_shutdown = after_shutdown;

    struct aws_byte_buf read_tag = aws_byte_buf_from_c_str("I'm a little teapot.");
    struct aws_byte_buf write_tag = aws_byte_buf_from_c_str("I'm a big teapot");
    /* Initialize the handler for client with small window, and shutdown the server  */
    struct aws_channel_handler *client_rw_handler = rw_handler_new(
        allocator,
        s_on_client_recive_shutdown_with_cache_data,
        s_tls_test_handle_write,
        true,
        write_tag.len / 2,
        &s_server_client_tester.client_rw_args);
    ASSERT_NOT_NULL(client_rw_handler);

    struct aws_channel_handler *server_rw_handler = rw_handler_new(
        allocator,
        s_tls_test_handle_read,
        s_tls_test_handle_write,
        true,
        SIZE_MAX,
        &s_server_client_tester.server_rw_args);
    ASSERT_NOT_NULL(server_rw_handler);

    s_server_client_tester.server_args.rw_handler = server_rw_handler;
    s_server_client_tester.client_args.rw_handler = client_rw_handler;

    g_aws_channel_max_fragment_size = 4096;
    ASSERT_SUCCESS(s_set_socket_channel(&s_server_client_tester));

    /* Server sends data to client */
    rw_handler_write(
        s_server_client_tester.server_args.rw_handler, s_server_client_tester.server_args.rw_slot, &read_tag);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable,
        &c_tester.mutex,
        s_tls_test_read_predicate,
        &s_server_client_tester.client_rw_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    if (s_server_client_tester.window_update_after_shutdown) {
        rw_handler_trigger_increment_read_window(
            s_server_client_tester.client_args.rw_handler, s_server_client_tester.client_args.rw_slot, 100);
    }

    /* Make sure client also shutdown without error. */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable,
        &c_tester.mutex,
        s_tls_channel_shutdown_predicate,
        &s_server_client_tester.client_args));
    aws_mutex_unlock(&c_tester.mutex);

    s_server_client_tester.client_rw_args.invocation_happened = false;

    ASSERT_INT_EQUALS(2, s_server_client_tester.client_rw_args.read_invocations);

    ASSERT_BIN_ARRAYS_EQUALS(
        read_tag.buffer,
        read_tag.len,
        s_server_client_tester.client_rw_args.received_message.buffer,
        s_server_client_tester.client_rw_args.received_message.len);

    /* clean up */
    /*no shutdown on the client necessary here (it should have been triggered by shutting down the other side). just
     * wait for the event to fire. */
    ASSERT_SUCCESS(s_tls_channel_server_client_tester_cleanup());

    return AWS_OP_SUCCESS;
}

static int s_tls_channel_shutdown_with_cache_test_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_tls_channel_shutdown_with_cache_test_helper(allocator, false);
}

AWS_TEST_CASE(tls_channel_shutdown_with_cache_test, s_tls_channel_shutdown_with_cache_test_fn)

static int s_tls_channel_shutdown_with_cache_window_update_after_shutdown_test_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;
    return s_tls_channel_shutdown_with_cache_test_helper(allocator, true);
}
AWS_TEST_CASE(
    tls_channel_shutdown_with_cache_window_update_after_shutdown_test,
    s_tls_channel_shutdown_with_cache_window_update_after_shutdown_test_fn)

struct default_host_callback_data {
    struct aws_host_address aaaa_address;
    struct aws_host_address a_address;
    bool has_aaaa_address;
    bool has_a_address;
    struct aws_condition_variable condition_variable;
    bool invoked;
};

static int s_verify_negotiation_fails_helper(
    struct aws_allocator *allocator,
    const struct aws_string *host_name,
    uint32_t port,
    struct aws_tls_ctx_options *client_ctx_options) {
    struct aws_tls_ctx *client_ctx = aws_tls_client_ctx_new(allocator, client_ctx_options);

    struct aws_tls_connection_options tls_client_conn_options;
    aws_tls_connection_options_init_from_ctx(&tls_client_conn_options, client_ctx);
    aws_tls_connection_options_set_callbacks(&tls_client_conn_options, s_tls_on_negotiated, NULL, NULL, NULL);
    struct aws_byte_cursor host_name_cur = aws_byte_cursor_from_string(host_name);
    aws_tls_connection_options_set_server_name(&tls_client_conn_options, allocator, &host_name_cur);

    struct tls_test_args outgoing_args = {
        .mutex = &c_tester.mutex,
        .allocator = allocator,
        .condition_variable = &c_tester.condition_variable,
        .error_invoked = false,
        .expects_error = true,
        .rw_handler = NULL,
        .server = false,
        .tls_levels_negotiated = 0,
        .desired_tls_levels = 1,
        .shutdown_finished = false,
    };

    tls_client_conn_options.user_data = &outgoing_args;

    struct aws_socket_options options;
    AWS_ZERO_STRUCT(options);
    options.connect_timeout_ms = BADSSL_TIMEOUT_MS;
    options.type = AWS_SOCKET_STREAM;
    options.domain = AWS_SOCKET_IPV4;

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = c_tester.el_group,
        .host_resolver = c_tester.resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);
    ASSERT_NOT_NULL(client_bootstrap);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = aws_string_c_str(host_name);
    channel_options.port = port;
    channel_options.socket_options = &options;
    channel_options.tls_options = &tls_client_conn_options;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* put this here to verify ownership semantics are correct. This should NOT cause a segfault. If it does, ya
     * done messed up. */
    aws_tls_connection_options_clean_up(&tls_client_conn_options);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    ASSERT_TRUE(outgoing_args.error_invoked);

    if (s_is_badssl_being_flaky(host_name, outgoing_args.last_error_code)) {
        return AWS_OP_SKIP;
    }

    ASSERT_TRUE(aws_error_code_is_tls(outgoing_args.last_error_code));

    aws_client_bootstrap_release(client_bootstrap);

    aws_tls_ctx_release(client_ctx);

    return AWS_OP_SUCCESS;
}

static int s_verify_negotiation_fails(
    struct aws_allocator *allocator,
    const struct aws_string *host_name,
    uint32_t port,
    void (*context_options_override_fn)(struct aws_tls_ctx_options *)) {

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct aws_tls_ctx_options client_ctx_options;
    aws_tls_ctx_options_init_default_client(&client_ctx_options, allocator);

    if (context_options_override_fn) {
        (*context_options_override_fn)(&client_ctx_options);
    }

    int ret = s_verify_negotiation_fails_helper(allocator, host_name, port, &client_ctx_options);
    if (ret == AWS_OP_SUCCESS) {
        aws_tls_ctx_options_clean_up(&client_ctx_options);
        ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

        return AWS_OP_SUCCESS;
    }
    return ret;
}

static int s_verify_negotiation_fails_with_ca_override(
    struct aws_allocator *allocator,
    const struct aws_string *host_name,
    const char *root_ca_path) {

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct aws_tls_ctx_options client_ctx_options;
    aws_tls_ctx_options_init_default_client(&client_ctx_options, allocator);

    ASSERT_SUCCESS(aws_tls_ctx_options_override_default_trust_store_from_path(&client_ctx_options, NULL, root_ca_path));

    int ret = s_verify_negotiation_fails_helper(allocator, host_name, 443, &client_ctx_options);
    if (ret == AWS_OP_SUCCESS) {
        aws_tls_ctx_options_clean_up(&client_ctx_options);
        ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

        return AWS_OP_SUCCESS;
    }
    return ret;
}

#    if defined(USE_S2N)
static int s_default_pki_path_exists_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    (void)allocator;

    ASSERT_TRUE(
        aws_determine_default_pki_dir() != NULL || aws_determine_default_pki_ca_file() != NULL,
        "Default TLS trust store not found on this system.");
    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(default_pki_path_exists, s_default_pki_path_exists_fn)
#    endif /* defined(USE_S2N) */

AWS_STATIC_STRING_FROM_LITERAL(s_expired_host_name, "expired.badssl.com");

static int s_tls_client_channel_negotiation_error_expired_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    return s_verify_negotiation_fails(allocator, s_expired_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_error_expired, s_tls_client_channel_negotiation_error_expired_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_wrong_host_name, "wrong.host.badssl.com");

static int s_tls_client_channel_negotiation_error_wrong_host_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    return s_verify_negotiation_fails(allocator, s_wrong_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_error_wrong_host, s_tls_client_channel_negotiation_error_wrong_host_fn)

static int s_tls_client_channel_negotiation_error_wrong_host_with_ca_override_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;

    return s_verify_negotiation_fails_with_ca_override(allocator, s_wrong_host_name, "DigiCertGlobalRootCA.crt.pem");
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_wrong_host_with_ca_override,
    s_tls_client_channel_negotiation_error_wrong_host_with_ca_override_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_self_signed_host_name, "self-signed.badssl.com");

static int s_tls_client_channel_negotiation_error_self_signed_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    return s_verify_negotiation_fails(allocator, s_self_signed_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_error_self_signed, s_tls_client_channel_negotiation_error_self_signed_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_untrusted_root_host_name, "untrusted-root.badssl.com");

static int s_tls_client_channel_negotiation_error_untrusted_root_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    return s_verify_negotiation_fails(allocator, s_untrusted_root_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_untrusted_root,
    s_tls_client_channel_negotiation_error_untrusted_root_fn);

AWS_STATIC_STRING_FROM_LITERAL(s_amazon_host_name, "www.amazon.com");

/* negotiation should fail. www.amazon.com is obviously trusted by the default trust store,
 * but we've overridden the default trust store */
static int s_tls_client_channel_negotiation_error_untrusted_root_due_to_ca_override_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;

    return s_verify_negotiation_fails_with_ca_override(allocator, s_amazon_host_name, "ca_root.crt");
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_untrusted_root_due_to_ca_override,
    s_tls_client_channel_negotiation_error_untrusted_root_due_to_ca_override_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_broken_crypto_rc4_host_name, "rc4.badssl.com");

static int s_tls_client_channel_negotiation_error_broken_crypto_rc4_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_broken_crypto_rc4_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_broken_crypto_rc4,
    s_tls_client_channel_negotiation_error_broken_crypto_rc4_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_broken_crypto_rc4_md5_host_name, "rc4-md5.badssl.com");

static int s_tls_client_channel_negotiation_error_broken_crypto_rc4_md5_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_broken_crypto_rc4_md5_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_broken_crypto_rc4_md5,
    s_tls_client_channel_negotiation_error_broken_crypto_rc4_md5_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_broken_crypto_dh480_host_name, "dh480.badssl.com");

static int s_tls_client_channel_negotiation_error_broken_crypto_dh480_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_broken_crypto_dh480_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_broken_crypto_dh480,
    s_tls_client_channel_negotiation_error_broken_crypto_dh480_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_broken_crypto_dh512_host_name, "dh512.badssl.com");

static int s_tls_client_channel_negotiation_error_broken_crypto_dh512_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_broken_crypto_dh512_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_broken_crypto_dh512,
    s_tls_client_channel_negotiation_error_broken_crypto_dh512_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_broken_crypto_dh1024_host_name, "dh1024.badssl.com");

static int s_tls_client_channel_negotiation_error_broken_crypto_dh1024_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_broken_crypto_dh1024_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_broken_crypto_dh1024,
    s_tls_client_channel_negotiation_error_broken_crypto_dh1024_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_broken_crypto_null_host_name, "null.badssl.com");

static int s_tls_client_channel_negotiation_error_broken_crypto_null_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_broken_crypto_null_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_broken_crypto_null,
    s_tls_client_channel_negotiation_error_broken_crypto_null_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_legacy_crypto_tls10_host_name, "tls-v1-0.badssl.com");

static void s_raise_tls_version_to_11(struct aws_tls_ctx_options *options) {
    aws_tls_ctx_options_set_minimum_tls_version(options, AWS_IO_TLSv1_2);
}

static int s_tls_client_channel_negotiation_error_legacy_crypto_tls10_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_legacy_crypto_tls10_host_name, 1010, &s_raise_tls_version_to_11);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_legacy_crypto_tls10,
    s_tls_client_channel_negotiation_error_legacy_crypto_tls10_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_legacy_crypto_tls11_host_name, "tls-v1-1.badssl.com");

static void s_raise_tls_version_to_12(struct aws_tls_ctx_options *options) {
    aws_tls_ctx_options_set_minimum_tls_version(options, AWS_IO_TLSv1_2);
}

static int s_tls_client_channel_negotiation_error_override_legacy_crypto_tls11_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_legacy_crypto_tls11_host_name, 1011, &s_raise_tls_version_to_12);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_override_legacy_crypto_tls11,
    s_tls_client_channel_negotiation_error_override_legacy_crypto_tls11_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_legacy_crypto_dh2048_host_name, "dh2048.badssl.com");

static int s_tls_client_channel_negotiation_error_legacy_crypto_dh2048_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_legacy_crypto_dh2048_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_legacy_crypto_dh2048,
    s_tls_client_channel_negotiation_error_legacy_crypto_dh2048_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_uncommon_no_subject_host_name, "no-subject.badssl.com");

static int s_tls_client_channel_negotiation_error_no_subject_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_uncommon_no_subject_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_error_no_subject, s_tls_client_channel_negotiation_error_no_subject_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_uncommon_no_common_name_host_name, "no-common-name.badssl.com");

static int s_tls_client_channel_negotiation_error_no_common_name_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_negotiation_fails(allocator, s_uncommon_no_common_name_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_no_common_name,
    s_tls_client_channel_negotiation_error_no_common_name_fn)

/* Test that, if the channel shuts down unexpectedly during tls negotiation, that the user code is still notified.
 * We make this happen by connecting to port 80 on s3 or amazon.com and attempting TLS,
 * which gets you hung up on after a few seconds */
static int s_tls_client_channel_negotiation_error_socket_closed_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    const char *host_name = "aws-crt-test-stuff.s3.amazonaws.com";
    uint32_t port = 80; /* Note: intentionally wrong and not 443 */

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct tls_opt_tester client_tls_opt_tester;
    struct aws_byte_cursor server_name = aws_byte_cursor_from_c_str(host_name);
    ASSERT_SUCCESS(s_tls_client_opt_tester_init(allocator, &client_tls_opt_tester, server_name));
    client_tls_opt_tester.opt.timeout_ms = 0; /* disable negotiation timeout for this test */

    struct tls_test_args outgoing_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &outgoing_args, false, &c_tester));

    struct aws_socket_options options = {
        .connect_timeout_ms = 10000, .type = AWS_SOCKET_STREAM, .domain = AWS_SOCKET_IPV4};

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = c_tester.el_group,
        .host_resolver = c_tester.resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);
    ASSERT_NOT_NULL(client_bootstrap);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = host_name;
    channel_options.port = port;
    channel_options.socket_options = &options;
    channel_options.tls_options = &client_tls_opt_tester.opt;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* Wait for setup to complete */
    aws_mutex_lock(&c_tester.mutex);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &outgoing_args));

    /* Assert that setup failed, and that it failed for reasons unrelated to the tls-handler. */
    ASSERT_INT_EQUALS(0, outgoing_args.tls_levels_negotiated);
    ASSERT_TRUE(outgoing_args.error_invoked);
    ASSERT_INT_EQUALS(AWS_IO_SOCKET_CLOSED, outgoing_args.last_error_code);

    aws_mutex_unlock(&c_tester.mutex);

    /* Clean up */
    aws_client_bootstrap_release(client_bootstrap);

    s_tls_opt_tester_clean_up(&client_tls_opt_tester);
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_error_socket_closed,
    s_tls_client_channel_negotiation_error_socket_closed_fn);

static int s_verify_good_host(
    struct aws_allocator *allocator,
    const struct aws_string *host_name,
    uint32_t port,
    void (*override_tls_options_fn)(struct aws_tls_ctx_options *)) {

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct tls_test_args outgoing_args = {
        .mutex = &c_tester.mutex,
        .allocator = allocator,
        .condition_variable = &c_tester.condition_variable,
        .error_invoked = 0,
        .rw_handler = NULL,
        .server = false,
        .tls_levels_negotiated = 0,
        .desired_tls_levels = 1,
        .shutdown_finished = false,
    };

    struct aws_tls_ctx_options client_ctx_options;
    AWS_ZERO_STRUCT(client_ctx_options);
    aws_tls_ctx_options_set_verify_peer(&client_ctx_options, true);
    aws_tls_ctx_options_init_default_client(&client_ctx_options, allocator);
    aws_tls_ctx_options_set_alpn_list(&client_ctx_options, "http/1.1");

    if (override_tls_options_fn) {
        (*override_tls_options_fn)(&client_ctx_options);
    }

    struct aws_tls_ctx *client_ctx = aws_tls_client_ctx_new(allocator, &client_ctx_options);
    ASSERT_NOT_NULL(client_ctx);

    struct aws_tls_connection_options tls_client_conn_options;
    aws_tls_connection_options_init_from_ctx(&tls_client_conn_options, client_ctx);
    aws_tls_connection_options_set_callbacks(&tls_client_conn_options, s_tls_on_negotiated, NULL, NULL, &outgoing_args);

    struct aws_byte_cursor host_name_cur = aws_byte_cursor_from_string(host_name);
    aws_tls_connection_options_set_server_name(&tls_client_conn_options, allocator, &host_name_cur);
    aws_tls_connection_options_set_alpn_list(&tls_client_conn_options, allocator, "http/1.1");

    struct aws_socket_options options;
    AWS_ZERO_STRUCT(options);
    options.connect_timeout_ms = BADSSL_TIMEOUT_MS;
    options.type = AWS_SOCKET_STREAM;
    options.domain = AWS_SOCKET_IPV4;

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = c_tester.el_group,
        .host_resolver = c_tester.resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);
    ASSERT_NOT_NULL(client_bootstrap);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = aws_string_c_str(host_name);
    channel_options.port = port;
    channel_options.socket_options = &options;
    channel_options.tls_options = &tls_client_conn_options;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* put this here to verify ownership semantics are correct. This should NOT cause a segfault. If it does, ya
     * done messed up. */
    aws_tls_connection_options_clean_up(&tls_client_conn_options);

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    if (s_is_badssl_being_flaky(host_name, outgoing_args.last_error_code)) {
        return AWS_OP_SKIP;
    }

    ASSERT_FALSE(outgoing_args.error_invoked);
    struct aws_byte_buf expected_protocol = aws_byte_buf_from_c_str("http/1.1");
    /* check ALPN and SNI was properly negotiated */

    if (aws_tls_is_alpn_available() && client_ctx_options.verify_peer) {
        ASSERT_BIN_ARRAYS_EQUALS(
            expected_protocol.buffer,
            expected_protocol.len,
            outgoing_args.negotiated_protocol.buffer,
            outgoing_args.negotiated_protocol.len);
    }

    ASSERT_BIN_ARRAYS_EQUALS(
        host_name->bytes, host_name->len, outgoing_args.server_name.buffer, outgoing_args.server_name.len);

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    aws_channel_shutdown(outgoing_args.channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    aws_client_bootstrap_release(client_bootstrap);

    aws_tls_ctx_release(client_ctx);
    aws_tls_ctx_options_clean_up(&client_ctx_options);
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}

static int s_verify_good_host_mqtt_connect(
    struct aws_allocator *allocator,
    const struct aws_string *host_name,
    uint32_t port,
    void (*override_tls_options_fn)(struct aws_tls_ctx_options *)) {

    struct aws_byte_buf cert_buf = {0};
    struct aws_byte_buf key_buf = {0};
    struct aws_byte_buf ca_buf = {0};

    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&cert_buf, allocator, "tls13_device.pem.crt"));
    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&key_buf, allocator, "tls13_device.key"));
    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&ca_buf, allocator, "tls13_server_root_ca.pem.crt"));

    struct aws_byte_cursor cert_cur = aws_byte_cursor_from_buf(&cert_buf);
    struct aws_byte_cursor key_cur = aws_byte_cursor_from_buf(&key_buf);
    struct aws_byte_cursor ca_cur = aws_byte_cursor_from_buf(&ca_buf);

    aws_io_library_init(allocator);

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    uint8_t outgoing_received_message[128] = {0};

    struct tls_test_rw_args outgoing_rw_args;
    ASSERT_SUCCESS(s_tls_rw_args_init(
        &outgoing_rw_args,
        &c_tester,
        aws_byte_buf_from_empty_array(outgoing_received_message, sizeof(outgoing_received_message))));

    struct tls_test_args outgoing_args = {
        .mutex = &c_tester.mutex,
        .allocator = allocator,
        .condition_variable = &c_tester.condition_variable,
        .error_invoked = 0,
        .rw_handler = NULL,
        .server = false,
        .tls_levels_negotiated = 0,
        .desired_tls_levels = 1,
        .shutdown_finished = false,
    };

    struct aws_tls_ctx_options tls_options = {0};
    AWS_ZERO_STRUCT(tls_options);

    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS == aws_tls_ctx_options_init_client_mtls(&tls_options, allocator, &cert_cur, &key_cur));

    /* tls13_server_root_ca.pem.crt is self-signed, so peer verification fails without additional OS configuration. */
    aws_tls_ctx_options_set_verify_peer(&tls_options, false);
    aws_tls_ctx_options_set_alpn_list(&tls_options, "x-amzn-mqtt-ca");

    if (override_tls_options_fn) {
        (*override_tls_options_fn)(&tls_options);
    }

    struct aws_tls_ctx *tls_context = aws_tls_client_ctx_new(allocator, &tls_options);
    ASSERT_NOT_NULL(tls_context);

    struct aws_tls_connection_options tls_client_conn_options;
    aws_tls_connection_options_init_from_ctx(&tls_client_conn_options, tls_context);
    aws_tls_connection_options_set_callbacks(&tls_client_conn_options, s_tls_on_negotiated, NULL, NULL, &outgoing_args);

    aws_tls_ctx_options_override_default_trust_store(&tls_options, &ca_cur);

    struct aws_byte_cursor host_name_cur = aws_byte_cursor_from_string(host_name);
    aws_tls_connection_options_set_server_name(&tls_client_conn_options, allocator, &host_name_cur);
    aws_tls_connection_options_set_alpn_list(&tls_client_conn_options, allocator, "x-amzn-mqtt-ca");

    struct aws_socket_options options;
    AWS_ZERO_STRUCT(options);
    options.connect_timeout_ms = 10000;
    options.type = AWS_SOCKET_STREAM;
    options.domain = AWS_SOCKET_IPV4;

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = c_tester.el_group,
        .host_resolver = c_tester.resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);
    ASSERT_NOT_NULL(client_bootstrap);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = aws_string_c_str(host_name);
    channel_options.port = port;
    channel_options.socket_options = &options;
    channel_options.tls_options = &tls_client_conn_options;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* put this here to verify ownership semantics are correct. This should NOT cause a segfault. If it does, ya
     * done messed up. */
    aws_tls_connection_options_clean_up(&tls_client_conn_options);

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    ASSERT_FALSE(outgoing_args.error_invoked);
    struct aws_byte_buf expected_protocol = aws_byte_buf_from_c_str("x-amzn-mqtt-ca");
    /* check ALPN and SNI was properly negotiated */
    if (aws_tls_is_alpn_available() && tls_options.verify_peer) {
        ASSERT_BIN_ARRAYS_EQUALS(
            expected_protocol.buffer,
            expected_protocol.len,
            outgoing_args.negotiated_protocol.buffer,
            outgoing_args.negotiated_protocol.len);
    }

    ASSERT_BIN_ARRAYS_EQUALS(
        host_name->bytes, host_name->len, outgoing_args.server_name.buffer, outgoing_args.server_name.len);

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    aws_channel_shutdown(outgoing_args.channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    /* cleanups */
    aws_byte_buf_clean_up(&cert_buf);
    aws_byte_buf_clean_up(&key_buf);
    aws_byte_buf_clean_up(&ca_buf);
    aws_tls_ctx_release(tls_context);
    aws_tls_ctx_options_clean_up(&tls_options);
    aws_client_bootstrap_release(client_bootstrap);
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}

static int s_tls_client_channel_negotiation_success_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_amazon_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success, s_tls_client_channel_negotiation_success_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_badssl_ecc256_host_name, "ecc256.badssl.com");

static int s_tls_client_channel_negotiation_success_ecc256_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_badssl_ecc256_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_ecc256, s_tls_client_channel_negotiation_success_ecc256_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_badssl_ecc384_host_name, "ecc384.badssl.com");

static int s_tls_client_channel_negotiation_success_ecc384_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_badssl_ecc384_host_name, 443, NULL);
}
AWS_TEST_CASE(tls_client_channel_negotiation_success_ecc384, s_tls_client_channel_negotiation_success_ecc384_fn)

#    ifdef _WIN32

static int s_tls_client_channel_negotiation_success_ecc384_SCHANNEL_CREDS_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;

    // Force using SCHANNEL_CREDS for testing
    aws_windows_force_schannel_creds(true);
    s_verify_good_host(allocator, s_badssl_ecc384_host_name, 443, NULL);
    aws_windows_force_schannel_creds(false); // reset
    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_ecc384_deprecated,
    s_tls_client_channel_negotiation_success_ecc384_SCHANNEL_CREDS_fn)
#    endif

static void s_raise_tls_version_to_13(struct aws_tls_ctx_options *options) {
    aws_tls_ctx_options_set_minimum_tls_version(options, AWS_IO_TLSv1_3);
}

AWS_STATIC_STRING_FROM_LITERAL(s_aws_ecc384_host_name, "127.0.0.1");
static int s_tls_client_channel_negotiation_success_mtls_tls1_3_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host_mqtt_connect(allocator, s_aws_ecc384_host_name, 59443, s_raise_tls_version_to_13);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_mtls_tls1_3,
    s_tls_client_channel_negotiation_success_mtls_tls1_3_fn)

AWS_STATIC_STRING_FROM_LITERAL(s3_host_name, "s3.amazonaws.com");

static void s_disable_verify_peer(struct aws_tls_ctx_options *options) {
    aws_tls_ctx_options_set_verify_peer(options, false);
}

/* prove that connections complete even when verify_peer is false */
static int s_tls_client_channel_no_verify_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s3_host_name, 443, &s_disable_verify_peer);
}
AWS_TEST_CASE(tls_client_channel_no_verify, s_tls_client_channel_no_verify_fn)

/* Check all of the bad tls cases with verify_peer off.  Now they should succeed. */

static int s_tls_client_channel_negotiation_no_verify_expired_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_expired_host_name, 443, &s_disable_verify_peer);
}

AWS_TEST_CASE(tls_client_channel_negotiation_no_verify_expired, s_tls_client_channel_negotiation_no_verify_expired_fn)

static int s_tls_client_channel_negotiation_no_verify_wrong_host_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_wrong_host_name, 443, &s_disable_verify_peer);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_no_verify_wrong_host,
    s_tls_client_channel_negotiation_no_verify_wrong_host_fn)

static int s_tls_client_channel_negotiation_no_verify_self_signed_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_self_signed_host_name, 443, &s_disable_verify_peer);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_no_verify_self_signed,
    s_tls_client_channel_negotiation_no_verify_self_signed_fn)

static int s_tls_client_channel_negotiation_no_verify_untrusted_root_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_untrusted_root_host_name, 443, &s_disable_verify_peer);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_no_verify_untrusted_root,
    s_tls_client_channel_negotiation_no_verify_untrusted_root_fn)

static void s_lower_tls_version_to_tls10(struct aws_tls_ctx_options *options) {
    aws_tls_ctx_options_set_minimum_tls_version(options, AWS_IO_TLSv1);
}

static int s_tls_client_channel_negotiation_override_legacy_crypto_tls10_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_legacy_crypto_tls10_host_name, 1010, &s_lower_tls_version_to_tls10);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_override_legacy_crypto_tls10,
    s_tls_client_channel_negotiation_override_legacy_crypto_tls10_fn)

static int s_tls_client_channel_negotiation_success_legacy_crypto_tls11_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_legacy_crypto_tls11_host_name, 1011, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_legacy_crypto_tls11,
    s_tls_client_channel_negotiation_success_legacy_crypto_tls11_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_uncommon_sha384_host_name, "sha384.badssl.com");

static int s_tls_client_channel_negotiation_success_sha384_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_uncommon_sha384_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_sha384, s_tls_client_channel_negotiation_success_sha384_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_uncommon_sha512_host_name, "sha512.badssl.com");

static int s_tls_client_channel_negotiation_success_sha512_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_uncommon_sha512_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_sha512, s_tls_client_channel_negotiation_success_sha512_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_uncommon_rsa8192_host_name, "rsa8192.badssl.com");

static int s_tls_client_channel_negotiation_success_rsa8192_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_uncommon_rsa8192_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_rsa8192, s_tls_client_channel_negotiation_success_rsa8192_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_uncommon_incomplete_chain_host_name, "incomplete-chain.badssl.com");

static int s_tls_client_channel_negotiation_success_no_verify_incomplete_chain_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_uncommon_incomplete_chain_host_name, 443, s_disable_verify_peer);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_no_verify_incomplete_chain,
    s_tls_client_channel_negotiation_success_no_verify_incomplete_chain_fn)

static int s_tls_client_channel_negotiation_success_no_verify_no_subject_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_uncommon_no_subject_host_name, 443, s_disable_verify_peer);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_no_verify_no_subject,
    s_tls_client_channel_negotiation_success_no_verify_no_subject_fn)

static int s_tls_client_channel_negotiation_success_no_verify_no_common_name_fn(
    struct aws_allocator *allocator,
    void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_uncommon_no_common_name_host_name, 443, s_disable_verify_peer);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_no_verify_no_common_name,
    s_tls_client_channel_negotiation_success_no_verify_no_common_name_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_common_tls12_host_name, "tls-v1-2.badssl.com");

static int s_tls_client_channel_negotiation_success_tls12_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_common_tls12_host_name, 1012, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_tls12, s_tls_client_channel_negotiation_success_tls12_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_common_sha256_host_name, "sha256.badssl.com");

static int s_tls_client_channel_negotiation_success_sha256_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_common_sha256_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_sha256, s_tls_client_channel_negotiation_success_sha256_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_common_rsa2048_host_name, "rsa2048.badssl.com");

static int s_tls_client_channel_negotiation_success_rsa2048_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_common_rsa2048_host_name, 443, NULL);
}

AWS_TEST_CASE(tls_client_channel_negotiation_success_rsa2048, s_tls_client_channel_negotiation_success_rsa2048_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_common_extended_validation_host_name, "extended-validation.badssl.com");

static int s_tls_client_channel_negotiation_success_extended_validation_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_common_extended_validation_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_extended_validation,
    s_tls_client_channel_negotiation_success_extended_validation_fn)

AWS_STATIC_STRING_FROM_LITERAL(s_common_mozilla_modern_host_name, "mozilla-modern.badssl.com");

static int s_tls_client_channel_negotiation_success_mozilla_modern_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    return s_verify_good_host(allocator, s_common_mozilla_modern_host_name, 443, NULL);
}

AWS_TEST_CASE(
    tls_client_channel_negotiation_success_mozilla_modern,
    s_tls_client_channel_negotiation_success_mozilla_modern_fn)

static void s_reset_arg_state(struct tls_test_args *setup_test_args) {
    setup_test_args->tls_levels_negotiated = 0;
    setup_test_args->shutdown_finished = false;
    setup_test_args->creation_callback_invoked = false;
    setup_test_args->setup_callback_invoked = false;
}

static int s_tls_server_multiple_connections_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct tls_test_args outgoing_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &outgoing_args, false, &c_tester));

    struct tls_test_args incoming_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &incoming_args, true, &c_tester));

    struct tls_local_server_tester local_server_tester;
    ASSERT_SUCCESS(s_tls_local_server_tester_init(
        allocator, &local_server_tester, &incoming_args, &c_tester, false, "server.crt", "server.key"));

    struct tls_opt_tester client_tls_opt_tester;
    struct aws_byte_cursor server_name = aws_byte_cursor_from_c_str("localhost");
    ASSERT_SUCCESS(s_tls_client_opt_tester_init(allocator, &client_tls_opt_tester, server_name));
    aws_tls_connection_options_set_callbacks(
        &client_tls_opt_tester.opt, s_tls_on_negotiated, NULL, NULL, &outgoing_args);

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = c_tester.el_group,
        .host_resolver = c_tester.resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = local_server_tester.endpoint.address;
    channel_options.port = 0;
    channel_options.socket_options = &local_server_tester.socket_options;
    channel_options.tls_options = &client_tls_opt_tester.opt;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* wait for both ends to setup */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &incoming_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));
    ASSERT_FALSE(incoming_args.error_invoked);

    /* shut down */
    aws_channel_shutdown(incoming_args.channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &incoming_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    /* no shutdown on the client necessary here (it should have been triggered by shutting down the other side). just
     * wait for the event to fire. */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    /* connect again! */
    s_reset_arg_state(&outgoing_args);
    s_reset_arg_state(&incoming_args);

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* wait for both ends to setup */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &incoming_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));
    ASSERT_FALSE(incoming_args.error_invoked);

    /* shut down */
    aws_channel_shutdown(incoming_args.channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &incoming_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    /*no shutdown on the client necessary here (it should have been triggered by shutting down the other side). just
     * wait for the event to fire. */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));
    aws_server_bootstrap_destroy_socket_listener(local_server_tester.server_bootstrap, local_server_tester.listener);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_listener_destroy_predicate, &incoming_args));
    aws_mutex_unlock(&c_tester.mutex);

    /* clean up */
    ASSERT_SUCCESS(s_tls_opt_tester_clean_up(&client_tls_opt_tester));
    aws_client_bootstrap_release(client_bootstrap);
    ASSERT_SUCCESS(s_tls_local_server_tester_clean_up(&local_server_tester));
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}
AWS_TEST_CASE(tls_server_multiple_connections, s_tls_server_multiple_connections_fn)

struct shutdown_listener_tester {
    struct aws_socket *listener;
    struct aws_server_bootstrap *server_bootstrap;
    struct tls_test_args *outgoing_args; /* client args */
    struct aws_socket client_socket;
};

static bool s_client_socket_closed_predicate(void *user_data) {
    struct tls_test_args *args = user_data;
    return args->shutdown_finished;
}

static void s_close_client_socket_task(struct aws_task *task, void *arg, enum aws_task_status status) {
    (void)status;
    struct shutdown_listener_tester *tester = arg;

    /* Free task memory */
    aws_mem_release(tester->outgoing_args->allocator, task);

    /* Close socket and notify  */
    AWS_FATAL_ASSERT(aws_socket_close(&tester->client_socket) == AWS_OP_SUCCESS);

    AWS_FATAL_ASSERT(aws_mutex_lock(tester->outgoing_args->mutex) == AWS_OP_SUCCESS);
    tester->outgoing_args->shutdown_finished = true;
    AWS_FATAL_ASSERT(aws_mutex_unlock(tester->outgoing_args->mutex) == AWS_OP_SUCCESS);
    AWS_FATAL_ASSERT(aws_condition_variable_notify_one(tester->outgoing_args->condition_variable) == AWS_OP_SUCCESS);
}

static void s_on_client_connected_do_hangup(struct aws_socket *socket, int error_code, void *user_data) {
    AWS_FATAL_ASSERT(error_code == 0);
    struct shutdown_listener_tester *tester = user_data;
    tester->client_socket = *socket;

    /* wait 1 sec so server side has time to setup the channel, then close the socket */
    uint64_t run_at_ns;
    aws_event_loop_current_clock_time(socket->event_loop, &run_at_ns);
    run_at_ns += aws_timestamp_convert(1, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL);
    struct aws_task *close_client_socket_task =
        aws_mem_acquire(tester->outgoing_args->allocator, sizeof(struct aws_task));
    aws_task_init(close_client_socket_task, s_close_client_socket_task, tester, "wait_close_client_socket");
    aws_event_loop_schedule_task_future(socket->event_loop, close_client_socket_task, run_at_ns);
}

/* Test that server can handle a hangup in the middle of TLS negotiation */
static int s_tls_server_hangup_during_negotiation_fn(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct tls_test_args outgoing_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &outgoing_args, false, &c_tester));

    struct tls_test_args incoming_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &incoming_args, true, &c_tester));

    struct tls_local_server_tester local_server_tester;
    ASSERT_SUCCESS(s_tls_local_server_tester_init(
        allocator, &local_server_tester, &incoming_args, &c_tester, false, "server.crt", "server.key"));

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));

    struct shutdown_listener_tester *shutdown_tester =
        aws_mem_acquire(allocator, sizeof(struct shutdown_listener_tester));
    shutdown_tester->server_bootstrap = local_server_tester.server_bootstrap;
    shutdown_tester->listener = local_server_tester.listener;
    shutdown_tester->outgoing_args = &outgoing_args;

    /* Use a raw aws_socket for the client, instead of a full-blown TLS channel.
     * This lets us hang up on the server, instead of automatically going through with proper TLS negotiation */
    ASSERT_SUCCESS(aws_socket_init(&shutdown_tester->client_socket, allocator, &local_server_tester.socket_options));

    struct aws_socket_connect_options connect_options = {
        .remote_endpoint = &local_server_tester.endpoint,
        .event_loop = aws_event_loop_group_get_next_loop(c_tester.el_group),
        .on_connection_result = s_on_client_connected_do_hangup,
        .user_data = shutdown_tester};

    /* Upon connecting, immediately close the socket */
    ASSERT_SUCCESS(aws_socket_connect(&shutdown_tester->client_socket, &connect_options));

    /* Wait for client socket to close */
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_client_socket_closed_predicate, &outgoing_args));

    /* Destroy listener socket and wait for shutdown to complete */
    aws_server_bootstrap_destroy_socket_listener(shutdown_tester->server_bootstrap, shutdown_tester->listener);

    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_listener_destroy_predicate, &incoming_args));

    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));
    /* clean up */
    aws_socket_clean_up(&shutdown_tester->client_socket);
    aws_mem_release(allocator, shutdown_tester);
    /* cannot double free the listener */
    ASSERT_SUCCESS(s_tls_opt_tester_clean_up(&local_server_tester.server_tls_opt_tester));
    aws_server_bootstrap_release(local_server_tester.server_bootstrap);
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}
AWS_TEST_CASE(tls_server_hangup_during_negotiation, s_tls_server_hangup_during_negotiation_fn)

static void s_creation_callback_test_channel_creation_callback(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    (void)error_code;

    struct tls_test_args *setup_test_args = (struct tls_test_args *)user_data;

    setup_test_args->creation_callback_invoked = true;
    setup_test_args->channel = channel;

    struct aws_crt_statistics_handler *stats_handler = aws_statistics_handler_new_test(bootstrap->allocator);
    aws_atomic_store_ptr(&c_tester.stats_handler, stats_handler);

    aws_channel_set_statistics_handler(channel, stats_handler);
}

static struct aws_event_loop *s_default_new_event_loop(
    struct aws_allocator *allocator,
    const struct aws_event_loop_options *options,
    void *user_data) {

    (void)user_data;
    return aws_event_loop_new(allocator, options);
}

static int s_statistic_test_clock_fn(uint64_t *timestamp) {
    *timestamp = aws_atomic_load_int(&c_tester.current_time_ns);

    return AWS_OP_SUCCESS;
}

static int s_tls_common_tester_statistics_init(struct aws_allocator *allocator, struct tls_common_tester *tester) {

    aws_io_library_init(allocator);

    AWS_ZERO_STRUCT(*tester);

    struct aws_mutex mutex = AWS_MUTEX_INIT;
    struct aws_condition_variable condition_variable = AWS_CONDITION_VARIABLE_INIT;
    tester->mutex = mutex;
    tester->condition_variable = condition_variable;
    aws_atomic_store_int(&tester->current_time_ns, 0);
    aws_atomic_store_ptr(&tester->stats_handler, NULL);

    struct aws_event_loop_group_options elg_options = {
        .loop_count = 1,
        .clock_override = s_statistic_test_clock_fn,
    };
    tester->el_group = aws_event_loop_group_new_internal(allocator, &elg_options, s_default_new_event_loop, NULL);

    struct aws_host_resolver_default_options resolver_options = {
        .el_group = tester->el_group,
        .max_entries = 1,
    };
    tester->resolver = aws_host_resolver_new_default(allocator, &resolver_options);

    return AWS_OP_SUCCESS;
}

static bool s_stats_processed_predicate(void *user_data) {
    struct aws_crt_statistics_handler *stats_handler = user_data;
    struct aws_statistics_handler_test_impl *stats_impl = stats_handler->impl;

    return stats_impl->total_bytes_read > 0 && stats_impl->total_bytes_written > 0 &&
           stats_impl->tls_status != AWS_TLS_NEGOTIATION_STATUS_NONE;
}

static int s_tls_channel_statistics_test(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    aws_io_library_init(allocator);

    ASSERT_SUCCESS(s_tls_common_tester_statistics_init(allocator, &c_tester));

    struct aws_byte_buf read_tag = aws_byte_buf_from_c_str("This is some data.");
    struct aws_byte_buf write_tag = aws_byte_buf_from_c_str("Created from a blend of heirloom and cider apples");

    uint8_t incoming_received_message[128] = {0};
    uint8_t outgoing_received_message[128] = {0};

    struct tls_test_rw_args incoming_rw_args;
    ASSERT_SUCCESS(s_tls_rw_args_init(
        &incoming_rw_args,
        &c_tester,
        aws_byte_buf_from_empty_array(incoming_received_message, sizeof(incoming_received_message))));

    struct tls_test_rw_args outgoing_rw_args;
    ASSERT_SUCCESS(s_tls_rw_args_init(
        &outgoing_rw_args,
        &c_tester,
        aws_byte_buf_from_empty_array(outgoing_received_message, sizeof(outgoing_received_message))));

    struct tls_test_args outgoing_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &outgoing_args, false, &c_tester));

    struct tls_test_args incoming_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &incoming_args, true, &c_tester));

    struct tls_local_server_tester local_server_tester;
    ASSERT_SUCCESS(s_tls_local_server_tester_init(
        allocator, &local_server_tester, &incoming_args, &c_tester, false, "server.crt", "server.key"));

    struct aws_channel_handler *outgoing_rw_handler =
        rw_handler_new(allocator, s_tls_test_handle_read, s_tls_test_handle_write, true, 10000, &outgoing_rw_args);
    ASSERT_NOT_NULL(outgoing_rw_handler);

    struct aws_channel_handler *incoming_rw_handler =
        rw_handler_new(allocator, s_tls_test_handle_read, s_tls_test_handle_write, true, 10000, &incoming_rw_args);
    ASSERT_NOT_NULL(incoming_rw_handler);

    incoming_args.rw_handler = incoming_rw_handler;
    outgoing_args.rw_handler = outgoing_rw_handler;

    struct tls_opt_tester client_tls_opt_tester;
    struct aws_byte_cursor server_name = aws_byte_cursor_from_c_str("localhost");
    ASSERT_SUCCESS(s_tls_client_opt_tester_init(allocator, &client_tls_opt_tester, server_name));
    aws_tls_connection_options_set_callbacks(
        &client_tls_opt_tester.opt, s_tls_on_negotiated, NULL, NULL, &outgoing_args);

    struct aws_client_bootstrap_options bootstrap_options;
    AWS_ZERO_STRUCT(bootstrap_options);
    bootstrap_options.event_loop_group = c_tester.el_group;
    bootstrap_options.host_resolver = c_tester.resolver;

    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);

    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = local_server_tester.endpoint.address;
    channel_options.port = 0;
    channel_options.socket_options = &local_server_tester.socket_options;
    channel_options.tls_options = &client_tls_opt_tester.opt;
    channel_options.creation_callback = s_creation_callback_test_channel_creation_callback;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* put this here to verify ownership semantics are correct. This should NOT cause a segfault. If it does, ya
     * done messed up. */
    aws_tls_connection_options_clean_up(&client_tls_opt_tester.opt);
    /* wait for both ends to setup */
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &incoming_args));
    ASSERT_FALSE(incoming_args.error_invoked);

    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &outgoing_args));
    ASSERT_FALSE(outgoing_args.error_invoked);

    ASSERT_TRUE(outgoing_args.creation_callback_invoked);

    /* Do the IO operations */
    rw_handler_write(outgoing_args.rw_handler, outgoing_args.rw_slot, &write_tag);
    rw_handler_write(incoming_args.rw_handler, incoming_args.rw_slot, &read_tag);

    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_test_read_predicate, &incoming_rw_args));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_test_read_predicate, &outgoing_rw_args));

    uint64_t ms_to_ns = aws_timestamp_convert(1, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);

    aws_atomic_store_int(&c_tester.current_time_ns, (size_t)ms_to_ns);

    struct aws_crt_statistics_handler *stats_handler = aws_atomic_load_ptr(&c_tester.stats_handler);
    struct aws_statistics_handler_test_impl *stats_impl = stats_handler->impl;

    aws_mutex_lock(&stats_impl->lock);

    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &stats_impl->signal, &stats_impl->lock, s_stats_processed_predicate, stats_handler));

    ASSERT_TRUE(stats_impl->total_bytes_read >= read_tag.len);
    ASSERT_TRUE(stats_impl->total_bytes_written >= write_tag.len);
    ASSERT_TRUE(stats_impl->tls_status == AWS_TLS_NEGOTIATION_STATUS_SUCCESS);

    aws_mutex_unlock(&stats_impl->lock);

    aws_channel_shutdown(incoming_args.channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &incoming_args));

    /*no shutdown on the client necessary here (it should have been triggered by shutting down the other side). just
     * wait for the event to fire. */
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    aws_server_bootstrap_destroy_socket_listener(local_server_tester.server_bootstrap, local_server_tester.listener);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_listener_destroy_predicate, &incoming_args));
    aws_mutex_unlock(&c_tester.mutex);
    /* clean up */
    ASSERT_SUCCESS(s_tls_opt_tester_clean_up(&client_tls_opt_tester));
    ASSERT_SUCCESS(s_tls_local_server_tester_clean_up(&local_server_tester));
    aws_client_bootstrap_release(client_bootstrap);
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(tls_channel_statistics_test, s_tls_channel_statistics_test)

static int s_tls_certificate_chain_test(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    ASSERT_SUCCESS(s_tls_common_tester_init(allocator, &c_tester));

    struct tls_test_args outgoing_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &outgoing_args, false, &c_tester));

    struct tls_test_args incoming_args;
    ASSERT_SUCCESS(s_tls_test_arg_init(allocator, &incoming_args, true, &c_tester));

    struct tls_local_server_tester local_server_tester;
    ASSERT_SUCCESS(s_tls_local_server_tester_init(
        allocator, &local_server_tester, &incoming_args, &c_tester, false, "server_chain.crt", "server.key"));

    struct tls_opt_tester client_tls_opt_tester;
    struct aws_byte_cursor server_name = aws_byte_cursor_from_c_str("localhost");
    ASSERT_SUCCESS(s_tls_client_opt_tester_init(allocator, &client_tls_opt_tester, server_name));
    aws_tls_connection_options_set_callbacks(
        &client_tls_opt_tester.opt, s_tls_on_negotiated, NULL, NULL, &outgoing_args);

    struct aws_client_bootstrap_options bootstrap_options = {
        .event_loop_group = c_tester.el_group,
        .host_resolver = c_tester.resolver,
    };
    struct aws_client_bootstrap *client_bootstrap = aws_client_bootstrap_new(allocator, &bootstrap_options);

    struct aws_socket_channel_bootstrap_options channel_options;
    AWS_ZERO_STRUCT(channel_options);
    channel_options.bootstrap = client_bootstrap;
    channel_options.host_name = local_server_tester.endpoint.address;
    channel_options.port = 0;
    channel_options.socket_options = &local_server_tester.socket_options;
    channel_options.tls_options = &client_tls_opt_tester.opt;
    channel_options.setup_callback = s_tls_handler_test_client_setup_callback;
    channel_options.shutdown_callback = s_tls_handler_test_client_shutdown_callback;
    channel_options.user_data = &outgoing_args;

    /* connect! */
    ASSERT_SUCCESS(aws_client_bootstrap_new_socket_channel(&channel_options));

    /* wait for both ends to setup */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_setup_predicate, &incoming_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));
    ASSERT_FALSE(incoming_args.error_invoked);

    /* shut down */
    aws_channel_shutdown(incoming_args.channel, AWS_OP_SUCCESS);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &incoming_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    /* no shutdown on the client necessary here (it should have been triggered by shutting down the other side). just
     * wait for the event to fire. */
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_channel_shutdown_predicate, &outgoing_args));
    ASSERT_SUCCESS(aws_mutex_unlock(&c_tester.mutex));

    /* clean up */
    aws_server_bootstrap_destroy_socket_listener(local_server_tester.server_bootstrap, local_server_tester.listener);
    ASSERT_SUCCESS(aws_mutex_lock(&c_tester.mutex));
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &c_tester.condition_variable, &c_tester.mutex, s_tls_listener_destroy_predicate, &incoming_args));
    aws_mutex_unlock(&c_tester.mutex);

    ASSERT_SUCCESS(s_tls_opt_tester_clean_up(&client_tls_opt_tester));
    aws_client_bootstrap_release(client_bootstrap);
    ASSERT_SUCCESS(s_tls_local_server_tester_clean_up(&local_server_tester));
    ASSERT_SUCCESS(s_tls_common_tester_clean_up(&c_tester));

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(tls_certificate_chain_test, s_tls_certificate_chain_test)

///////////////////////////////////////////////////////////////

struct channel_stat_test_context {
    struct aws_allocator *allocator;
    struct tls_opt_tester *tls_tester;
    struct aws_mutex lock;
    struct aws_condition_variable signal;
    bool setup_completed;
    bool shutdown_completed;
    int error_code;
};

static void s_channel_setup_stat_test_context_init(
    struct channel_stat_test_context *context,
    struct aws_allocator *allocator,
    struct tls_opt_tester *tls_tester) {

    AWS_ZERO_STRUCT(*context);
    aws_mutex_init(&context->lock);
    aws_condition_variable_init(&context->signal);
    context->allocator = allocator;
    context->tls_tester = tls_tester;
}

static void s_channel_setup_stat_test_context_clean_up(struct channel_stat_test_context *context) {
    aws_mutex_clean_up(&context->lock);
    aws_condition_variable_clean_up(&context->signal);
}

static int s_dummy_process_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)handler;
    (void)slot;

    aws_mem_release(message->allocator, message);
    return AWS_OP_SUCCESS;
}

static int s_dummy_increment_read_window(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    size_t size) {
    (void)handler;
    (void)slot;
    (void)size;

    return AWS_OP_SUCCESS;
}

static int s_dummy_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately) {

    (void)handler;
    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, free_scarce_resources_immediately);
}

static size_t s_dummy_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;

    return 10000;
}

static size_t s_dummy_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;

    return 0;
}

static void s_dummy_destroy(struct aws_channel_handler *handler) {
    aws_mem_release(handler->alloc, handler);
}

static struct aws_channel_handler_vtable s_dummy_handler_vtable = {
    .process_read_message = s_dummy_process_message,
    .process_write_message = s_dummy_process_message,
    .increment_read_window = s_dummy_increment_read_window,
    .shutdown = s_dummy_shutdown,
    .initial_window_size = s_dummy_initial_window_size,
    .message_overhead = s_dummy_message_overhead,
    .destroy = s_dummy_destroy,
};

static struct aws_channel_handler *aws_channel_handler_new_dummy(struct aws_allocator *allocator) {
    struct aws_channel_handler *handler = aws_mem_acquire(allocator, sizeof(struct aws_channel_handler));
    handler->alloc = allocator;
    handler->vtable = &s_dummy_handler_vtable;
    handler->impl = NULL;

    return handler;
}

static bool s_setup_completed_predicate(void *arg) {
    struct channel_stat_test_context *context = (struct channel_stat_test_context *)arg;
    return context->setup_completed;
}

static bool s_shutdown_completed_predicate(void *arg) {
    struct channel_stat_test_context *context = (struct channel_stat_test_context *)arg;
    return context->shutdown_completed;
}

static void s_on_shutdown_completed(struct aws_channel *channel, int error_code, void *user_data) {
    (void)channel;
    struct channel_stat_test_context *context = (struct channel_stat_test_context *)user_data;

    aws_mutex_lock(&context->lock);
    context->shutdown_completed = true;
    context->error_code = error_code;
    aws_mutex_unlock(&context->lock);

    aws_condition_variable_notify_one(&context->signal);
}

static const int s_tls_timeout_ms = 1000;

static void s_on_setup_completed(struct aws_channel *channel, int error_code, void *user_data) {
    (void)channel;
    struct channel_stat_test_context *context = (struct channel_stat_test_context *)user_data;

    /* attach a dummy channel handler */
    struct aws_channel_slot *dummy_slot = aws_channel_slot_new(channel);

    struct aws_channel_handler *dummy_handler = aws_channel_handler_new_dummy(context->allocator);
    aws_channel_slot_set_handler(dummy_slot, dummy_handler);

    /* attach a tls channel handler and start negotiation */
    aws_channel_setup_client_tls(dummy_slot, &context->tls_tester->opt);

    aws_mutex_lock(&context->lock);
    context->error_code = error_code;
    context->setup_completed = true;
    aws_mutex_unlock(&context->lock);
    aws_condition_variable_notify_one(&context->signal);
}

static int s_test_tls_negotiation_timeout(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    aws_io_library_init(allocator);

    struct aws_event_loop *event_loop = aws_event_loop_new_default(allocator, aws_high_res_clock_get_ticks);

    ASSERT_SUCCESS(aws_event_loop_run(event_loop));

    struct tls_opt_tester tls_test_context;
    s_tls_client_opt_tester_init(allocator, &tls_test_context, aws_byte_cursor_from_c_str("derp.com"));
    tls_test_context.opt.timeout_ms = s_tls_timeout_ms;

    struct channel_stat_test_context channel_context;
    s_channel_setup_stat_test_context_init(&channel_context, allocator, &tls_test_context);

    struct aws_channel_options args = {
        .on_setup_completed = s_on_setup_completed,
        .setup_user_data = &channel_context,
        .on_shutdown_completed = s_on_shutdown_completed,
        .shutdown_user_data = &channel_context,
        .event_loop = event_loop,
    };

    /* set up the channel */
    ASSERT_SUCCESS(aws_mutex_lock(&channel_context.lock));
    struct aws_channel *channel = aws_channel_new(allocator, &args);
    ASSERT_NOT_NULL(channel);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &channel_context.signal, &channel_context.lock, s_setup_completed_predicate, &channel_context));
    aws_mutex_unlock(&channel_context.lock);

    /* wait for the timeout */
    aws_thread_current_sleep(aws_timestamp_convert(s_tls_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL));

    aws_mutex_lock(&channel_context.lock);
    ASSERT_SUCCESS(aws_condition_variable_wait_pred(
        &channel_context.signal, &channel_context.lock, s_shutdown_completed_predicate, &channel_context));

    ASSERT_TRUE(channel_context.error_code == AWS_IO_TLS_NEGOTIATION_TIMEOUT);

    aws_mutex_unlock(&channel_context.lock);

    aws_channel_destroy(channel);
    aws_event_loop_destroy(event_loop);

    s_tls_opt_tester_clean_up(&tls_test_context);

    s_channel_setup_stat_test_context_clean_up(&channel_context);

    aws_io_library_clean_up();

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(test_tls_negotiation_timeout, s_test_tls_negotiation_timeout)

struct import_info {
    struct aws_allocator *allocator;
    struct aws_byte_buf cert_buf;
    struct aws_byte_buf key_buf;
    struct aws_thread thread;
    struct aws_tls_ctx *tls;
};

static void s_import_cert(void *ctx) {
    (void)ctx;
    struct import_info *import = ctx;
    struct aws_byte_cursor cert_cur = aws_byte_cursor_from_buf(&import->cert_buf);
    struct aws_byte_cursor key_cur = aws_byte_cursor_from_buf(&import->key_buf);
    struct aws_tls_ctx_options tls_options = {0};
    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS == aws_tls_ctx_options_init_client_mtls(&tls_options, import->allocator, &cert_cur, &key_cur));

    /* import happens in here */
    import->tls = aws_tls_client_ctx_new(import->allocator, &tls_options);
    AWS_FATAL_ASSERT(import->tls);

    aws_tls_ctx_options_clean_up(&tls_options);
}

#    define NUM_PAIRS 2
static int s_test_concurrent_cert_import(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    aws_io_library_init(allocator);

    AWS_VARIABLE_LENGTH_ARRAY(struct import_info, imports, NUM_PAIRS);

    /* setup, note that all I/O should be before the threads are launched */
    for (size_t idx = 0; idx < NUM_PAIRS; ++idx) {
        struct import_info *import = &imports[idx];
        import->allocator = allocator;

        char filename[1024];
        snprintf(filename, sizeof(filename), "testcert%u.pem", (uint32_t)idx);
        ASSERT_SUCCESS(aws_byte_buf_init_from_file(&import->cert_buf, import->allocator, filename));

        snprintf(filename, sizeof(filename), "testkey.pem");
        ASSERT_SUCCESS(aws_byte_buf_init_from_file(&import->key_buf, import->allocator, filename));

        struct aws_thread *thread = &import->thread;
        ASSERT_SUCCESS(aws_thread_init(thread, allocator));
    }

    /* run threads */
    const struct aws_thread_options *options = aws_default_thread_options();
    for (size_t idx = 0; idx < NUM_PAIRS; ++idx) {
        struct import_info *import = &imports[idx];
        struct aws_thread *thread = &import->thread;
        ASSERT_SUCCESS(aws_thread_launch(thread, s_import_cert, import, options));
    }

    /* join and clean up */
    for (size_t idx = 0; idx < NUM_PAIRS; ++idx) {
        struct import_info *import = &imports[idx];
        struct aws_thread *thread = &import->thread;
        ASSERT_SUCCESS(aws_thread_join(thread));
        aws_tls_ctx_release(import->tls);
        aws_byte_buf_clean_up(&import->cert_buf);
        aws_byte_buf_clean_up(&import->key_buf);
    }

    aws_io_library_clean_up();

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(test_concurrent_cert_import, s_test_concurrent_cert_import)

static int s_test_duplicate_cert_import(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;

    aws_io_library_init(allocator);
    struct aws_byte_buf cert_buf = {0};
    struct aws_byte_buf key_buf = {0};

#    if !defined(AWS_USE_SECITEM)

    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&cert_buf, allocator, "testcert0.pem"));
    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&key_buf, allocator, "testkey.pem"));
    struct aws_byte_cursor cert_cur = aws_byte_cursor_from_buf(&cert_buf);
    struct aws_byte_cursor key_cur = aws_byte_cursor_from_buf(&key_buf);
    struct aws_tls_ctx_options tls_options = {0};
    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS == aws_tls_ctx_options_init_client_mtls(&tls_options, allocator, &cert_cur, &key_cur));

    /* import happens in here */
    struct aws_tls_ctx *tls = aws_tls_client_ctx_new(allocator, &tls_options);
    AWS_FATAL_ASSERT(tls);
    aws_tls_ctx_release(tls);
    /* import the same certs twice */
    tls = aws_tls_client_ctx_new(allocator, &tls_options);
    AWS_FATAL_ASSERT(tls);
    aws_tls_ctx_release(tls);

    aws_tls_ctx_options_clean_up(&tls_options);
#    endif /* !AWS_USE_SECITEM */

    /* clean up */
    aws_byte_buf_clean_up(&cert_buf);
    aws_byte_buf_clean_up(&key_buf);
    aws_io_library_clean_up();

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(test_duplicate_cert_import, s_test_duplicate_cert_import)

static int s_tls_destroy_null_context(struct aws_allocator *allocator, void *ctx) {
    (void)allocator;
    (void)ctx;

    struct aws_tls_ctx *null_context = NULL;

    /* Verify that we don't crash. */
    aws_tls_ctx_release(null_context);

    return AWS_OP_SUCCESS;
}
AWS_TEST_CASE(tls_destroy_null_context, s_tls_destroy_null_context);

static int s_test_ecc_cert_import(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    (void)allocator;

#    ifndef AWS_OS_APPLE
    aws_io_library_init(allocator);

    struct aws_byte_buf cert_buf;
    struct aws_byte_buf key_buf;

    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&cert_buf, allocator, "ecc-cert.pem"));
    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&key_buf, allocator, "ecc-key.pem"));

    struct aws_byte_cursor cert_cur = aws_byte_cursor_from_buf(&cert_buf);
    struct aws_byte_cursor key_cur = aws_byte_cursor_from_buf(&key_buf);
    struct aws_tls_ctx_options tls_options = {0};
    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS == aws_tls_ctx_options_init_client_mtls(&tls_options, allocator, &cert_cur, &key_cur));

    /* import happens in here */
    struct aws_tls_ctx *tls_context = aws_tls_client_ctx_new(allocator, &tls_options);
    ASSERT_NOT_NULL(tls_context);

    aws_tls_ctx_release(tls_context);

    aws_tls_ctx_options_clean_up(&tls_options);

    aws_byte_buf_clean_up(&cert_buf);
    aws_byte_buf_clean_up(&key_buf);

    aws_io_library_clean_up();
#    endif /* AWS_OS_APPLE */

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(test_ecc_cert_import, s_test_ecc_cert_import)

static int s_test_pkcs8_import(struct aws_allocator *allocator, void *ctx) {
    (void)ctx;
    (void)allocator;

    aws_io_library_init(allocator);

    struct aws_byte_buf cert_buf;
    struct aws_byte_buf key_buf;

    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&cert_buf, allocator, "unittests.crt"));
    ASSERT_SUCCESS(aws_byte_buf_init_from_file(&key_buf, allocator, "unittests.p8"));

    struct aws_byte_cursor cert_cur = aws_byte_cursor_from_buf(&cert_buf);
    struct aws_byte_cursor key_cur = aws_byte_cursor_from_buf(&key_buf);
    struct aws_tls_ctx_options tls_options = {0};
    AWS_FATAL_ASSERT(
        AWS_OP_SUCCESS == aws_tls_ctx_options_init_client_mtls(&tls_options, allocator, &cert_cur, &key_cur));

    /* import happens in here */
    struct aws_tls_ctx *tls_context = aws_tls_client_ctx_new(allocator, &tls_options);
    ASSERT_NOT_NULL(tls_context);

    aws_tls_ctx_release(tls_context);

    aws_tls_ctx_options_clean_up(&tls_options);

    aws_byte_buf_clean_up(&cert_buf);
    aws_byte_buf_clean_up(&key_buf);

    aws_io_library_clean_up();

    return AWS_OP_SUCCESS;
}

AWS_TEST_CASE(test_pkcs8_import, s_test_pkcs8_import)

#endif /* BYO_CRYPTO */
