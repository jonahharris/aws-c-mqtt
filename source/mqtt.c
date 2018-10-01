/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/mqtt/mqtt.h>

#include <aws/mqtt/private/client_channel_handler.h>
#include <aws/mqtt/private/packets.h>
#include <aws/mqtt/private/utils.h>

#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>

#include <aws/common/task_scheduler.h>

#include <assert.h>

static void s_mqtt_subscription_impl_destroy(void *value) {
    struct aws_mqtt_subscription_impl *impl = value;

    aws_string_destroy((void *)impl->filter);
    aws_mem_release(impl->connection->allocator, impl);
}

/**
 * Channel has be initialized callback. Sets up channel handler and sends out CONNECT packet.
 * The on_connack callback is called with the CONNACK packet is received from the server.
 */
static int s_mqtt_client_init(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;

    if (error_code != AWS_OP_SUCCESS) {
        return AWS_OP_ERR;
    }

    struct aws_mqtt_client_connection *connection = user_data;

    /* Create the slot and handler */
    connection->slot = aws_channel_slot_new(channel);

    if (!connection->slot) {
        MQTT_CALL_CALLBACK(connection, on_connection_failed, aws_last_error());
        return AWS_OP_ERR;
    }

    aws_channel_slot_insert_end(channel, connection->slot);
    aws_channel_slot_set_handler(connection->slot, &connection->handler);

    /* Send the connect packet */
    struct aws_mqtt_packet_connect connect;
    aws_mqtt_packet_connect_init(
        &connect,
        aws_byte_cursor_from_buf(&connection->client_id),
        connection->clean_session,
        connection->keep_alive_time);

    struct aws_io_message *message = mqtt_get_message_for_packet(connection, &connect.fixed_header);
    if (!message) {
        MQTT_CALL_CALLBACK(connection, on_connection_failed, aws_last_error());
        return AWS_OP_ERR;
    }
    struct aws_byte_cursor message_cursor = {
        .ptr = message->message_data.buffer,
        .len = message->message_data.capacity,
    };
    if (aws_mqtt_packet_connect_encode(&message_cursor, &connect)) {
        MQTT_CALL_CALLBACK(connection, on_connection_failed, aws_last_error());
        return AWS_OP_ERR;
    }
    message->message_data.len = message->message_data.capacity - message_cursor.len;

    if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        MQTT_CALL_CALLBACK(connection, on_connection_failed, aws_last_error());
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_mqtt_client_shutdown(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    (void)channel;

    struct aws_mqtt_client_connection *connection = user_data;

    /* Alert the connection we've shutdown */
    MQTT_CALL_CALLBACK(connection, on_disconnect, error_code);

    return AWS_OP_SUCCESS;
}

static uint64_t s_hash_uint16_t(const void *item) {
    return *(uint16_t *)item;
}

static bool s_uint16_t_eq(const void *a, const void *b) {
    return *(uint16_t *)a == *(uint16_t *)b;
}

struct aws_mqtt_client_connection *aws_mqtt_client_connection_new(
    struct aws_allocator *allocator,
    struct aws_mqtt_client *client,
    struct aws_mqtt_client_connection_callbacks callbacks,
    struct aws_socket_endpoint *endpoint,
    struct aws_tls_connection_options *tls_options,
    struct aws_byte_cursor client_id,
    bool clean_session,
    uint16_t keep_alive_time) {

    assert(allocator);
    assert(client);
    assert(!tls_options || client->client_bootstrap->tls_ctx);

    struct aws_mqtt_client_connection *connection =
        aws_mem_acquire(allocator, sizeof(struct aws_mqtt_client_connection));

    if (!connection) {

        return NULL;
    }

    /* Initialize the client */
    AWS_ZERO_STRUCT(*connection);
    connection->allocator = allocator;
    connection->callbacks = callbacks;
    connection->state = AWS_MQTT_CLIENT_STATE_CONNECTING;
    connection->clean_session = clean_session;
    connection->keep_alive_time = keep_alive_time;

    struct aws_byte_buf client_id_buf = {
        .buffer = client_id.ptr,
        .len = client_id.len,
        .capacity = client_id.len,
    };
    aws_byte_buf_init_copy(allocator, &connection->client_id, &client_id_buf);

    /* Initialize the handler */
    connection->handler.alloc = allocator;
    connection->handler.vtable = aws_mqtt_get_client_channel_vtable();
    connection->handler.impl = connection;

    if (aws_hash_table_init(
            &connection->subscriptions,
            allocator,
            0,
            &aws_hash_string,
            &aws_string_eq,
            NULL,
            &s_mqtt_subscription_impl_destroy)) {

        goto handle_error;
    }

    if (aws_memory_pool_init(&connection->requests_pool, allocator, 32, sizeof(struct aws_mqtt_outstanding_request))) {

        goto handle_error;
    }
    if (aws_hash_table_init(
            &connection->outstanding_requests,
            allocator,
            sizeof(struct aws_mqtt_outstanding_request *),
            s_hash_uint16_t,
            s_uint16_t_eq,
            NULL,
            NULL)) {

        goto handle_error;
    }

    if (tls_options) {
        if (aws_client_bootstrap_new_tls_socket_channel(
                client->client_bootstrap,
                endpoint,
                client->socket_options,
                tls_options,
                &s_mqtt_client_init,
                &s_mqtt_client_shutdown,
                connection)) {

            goto handle_error;
        }
    } else {
        if (aws_client_bootstrap_new_socket_channel(
                client->client_bootstrap,
                endpoint,
                client->socket_options,
                &s_mqtt_client_init,
                &s_mqtt_client_shutdown,
                connection)) {

            goto handle_error;
        }
    }

    return connection;

handle_error:

    MQTT_CALL_CALLBACK(connection, on_connection_failed, aws_last_error());

    if (connection->subscriptions.p_impl) {
        aws_hash_table_clean_up(&connection->subscriptions);
    }

    if (connection->requests_pool.data_ptr) {
        aws_memory_pool_clean_up(&connection->requests_pool);
    }

    if (connection->outstanding_requests.p_impl) {
        aws_hash_table_clean_up(&connection->outstanding_requests);
    }

    aws_mem_release(allocator, connection);
    return NULL;
}

int aws_mqtt_client_connection_disconnect(struct aws_mqtt_client_connection *connection) {

    if (connection && connection->slot) {
        mqtt_disconnect_impl(connection, AWS_OP_SUCCESS);
    }

    return AWS_OP_SUCCESS;
}

static bool s_subscribe_send(uint16_t message_id, bool is_first_attempt, void *userdata) {
    (void)is_first_attempt;
    struct aws_mqtt_subscription_impl *subscription_impl = userdata;

    struct aws_io_message *message = NULL;

    /* Send the subscribe packet */
    struct aws_mqtt_packet_subscribe subscribe;
    if (aws_mqtt_packet_subscribe_init(&subscribe, subscription_impl->connection->allocator, message_id)) {
        goto handle_error;
    }
    if (aws_mqtt_packet_subscribe_add_topic(
            &subscribe, subscription_impl->subscription.topic_filter, subscription_impl->subscription.qos)) {
        goto handle_error;
    }

    message = mqtt_get_message_for_packet(subscription_impl->connection, &subscribe.fixed_header);
    if (!message) {
        goto handle_error;
    }
    struct aws_byte_cursor message_cursor = {
        .ptr = message->message_data.buffer,
        .len = message->message_data.capacity,
    };
    if (aws_mqtt_packet_subscribe_encode(&message_cursor, &subscribe)) {
        goto handle_error;
    }
    message->message_data.len = message->message_data.capacity - message_cursor.len;

    aws_mqtt_packet_subscribe_clean_up(&subscribe);

    if (aws_channel_slot_send_message(subscription_impl->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        goto handle_error;
    }

    return false;

handle_error:

    aws_mqtt_packet_subscribe_clean_up(&subscribe);

    if (message) {
        aws_channel_release_message_to_pool(subscription_impl->connection->slot->channel, message);
    }

    return true;
}

int aws_mqtt_client_subscribe(
    struct aws_mqtt_client_connection *connection,
    const struct aws_mqtt_subscription *subscription,
    aws_mqtt_publish_recieved_fn *callback,
    void *user_data) {

    assert(connection);

    int was_created = 0;

    struct aws_mqtt_subscription_impl *subscription_impl =
        aws_mem_acquire(connection->allocator, sizeof(struct aws_mqtt_subscription_impl));
    if (!subscription_impl) {
        goto handle_error;
    }

    subscription_impl->connection = connection;
    subscription_impl->callback = callback;
    subscription_impl->user_data = user_data;

    subscription_impl->filter = aws_string_new_from_array(
        connection->allocator, subscription->topic_filter.ptr, subscription->topic_filter.len);
    if (!subscription_impl->filter) {
        goto handle_error;
    }

    subscription_impl->subscription.qos = subscription->qos;
    subscription_impl->subscription.topic_filter = aws_byte_cursor_from_string(subscription_impl->filter);

    if (aws_hash_table_put(&connection->subscriptions, subscription_impl->filter, subscription_impl, &was_created)) {
        goto handle_error;
    }

    mqtt_create_request(subscription_impl->connection, &s_subscribe_send, NULL, subscription_impl);

    return AWS_OP_SUCCESS;

handle_error:

    if (subscription_impl) {
        if (subscription_impl->filter) {
            aws_string_destroy((void *)subscription_impl->filter);
        }
        aws_mem_release(connection->allocator, subscription_impl);
    }
    if (was_created) {
        aws_hash_table_remove(&connection->subscriptions, subscription_impl->filter, NULL, NULL);
    }
    return AWS_OP_ERR;
}

static bool s_unsubscribe_send(uint16_t message_id, bool is_first_attempt, void *userdata) {
    (void)is_first_attempt;

    struct aws_mqtt_subscription_impl *subscription_impl = userdata;
    struct aws_io_message *message = NULL;

    /* Send the unsubscribe packet */
    struct aws_mqtt_packet_unsubscribe unsubscribe;
    if (aws_mqtt_packet_unsubscribe_init(&unsubscribe, subscription_impl->connection->allocator, message_id)) {
        goto handle_error;
    }
    if (aws_mqtt_packet_unsubscribe_add_topic(&unsubscribe, subscription_impl->subscription.topic_filter)) {
        goto handle_error;
    }

    message = mqtt_get_message_for_packet(subscription_impl->connection, &unsubscribe.fixed_header);
    if (!message) {
        goto handle_error;
    }
    struct aws_byte_cursor message_cursor = {
        .ptr = message->message_data.buffer,
        .len = message->message_data.capacity,
    };
    if (aws_mqtt_packet_unsubscribe_encode(&message_cursor, &unsubscribe)) {
        goto handle_error;
    }
    message->message_data.len = message->message_data.capacity - message_cursor.len;

    aws_mqtt_packet_unsubscribe_clean_up(&unsubscribe);

    if (aws_channel_slot_send_message(subscription_impl->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {
        goto handle_error;
    }

    aws_hash_table_remove(&subscription_impl->connection->subscriptions, subscription_impl->filter, NULL, NULL);

    return false;

handle_error:

    aws_mqtt_packet_unsubscribe_clean_up(&unsubscribe);

    if (message) {
        aws_channel_release_message_to_pool(subscription_impl->connection->slot->channel, message);
    }

    aws_hash_table_remove(&subscription_impl->connection->subscriptions, subscription_impl->filter, NULL, NULL);

    return true;
}

int aws_mqtt_client_unsubscribe(struct aws_mqtt_client_connection *connection, const struct aws_byte_cursor *filter) {

    assert(connection);

    const struct aws_string *filter_str = aws_string_new_from_array(connection->allocator, filter->ptr, filter->len);
    if (!filter_str) {
        goto handle_error;
    }

    struct aws_hash_element *elem = NULL;
    aws_hash_table_find(&connection->subscriptions, filter_str, &elem);
    if (!elem) {
        goto handle_error;
    }

    /* Only needed this to do the lookup */
    aws_string_destroy((void *)filter_str);

    mqtt_create_request(connection, &s_unsubscribe_send, NULL, elem->value);

    return AWS_OP_SUCCESS;

handle_error:

    if (filter_str) {
        aws_string_destroy((void *)filter_str);
    }

    return AWS_OP_ERR;
}

struct publish_task_arg {
    struct aws_mqtt_client_connection *connection;
    struct aws_byte_cursor topic;
    enum aws_mqtt_qos qos;
    bool retain;
    struct aws_byte_cursor payload;

    aws_mqtt_publish_complete_fn *on_complete;
    void *userdata;
};

static bool s_publish_send(uint16_t message_id, bool is_first_attempt, void *userdata) {
    struct publish_task_arg *publish_arg = userdata;

    bool is_qos_0 = publish_arg->qos == AWS_MQTT_QOS_AT_MOST_ONCE;
    if (is_qos_0) {
        message_id = 0;
    }

    struct aws_mqtt_packet_publish publish;
    aws_mqtt_packet_publish_init(
        &publish,
        publish_arg->retain,
        publish_arg->qos,
        !is_first_attempt,
        publish_arg->topic,
        message_id,
        publish_arg->payload);

    struct aws_io_message *message = mqtt_get_message_for_packet(publish_arg->connection, &publish.fixed_header);
    if (!message) {
        goto handle_error;
    }
    struct aws_byte_cursor message_cursor = {
        .ptr = message->message_data.buffer,
        .len = message->message_data.capacity,
    };
    if (aws_mqtt_packet_publish_encode(&message_cursor, &publish)) {
        goto handle_error;
    }
    message->message_data.len = message->message_data.capacity - message_cursor.len;

    if (aws_channel_slot_send_message(publish_arg->connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {

        goto handle_error;
    }

    /* If QoS == 0, there will be no ack, so consider the request done now. */
    return is_qos_0;

handle_error:
    if (message) {
        aws_channel_release_message_to_pool(publish_arg->connection->slot->channel, message);
    }

    return true;
}

static void s_publish_complete(void *userdata) {
    struct publish_task_arg *publish_arg = userdata;

    if (publish_arg->on_complete) {
        publish_arg->on_complete(publish_arg->connection, publish_arg->userdata);
    }

    aws_mem_release(publish_arg->connection->allocator, publish_arg);
}

int aws_mqtt_client_publish(
    struct aws_mqtt_client_connection *connection,
    struct aws_byte_cursor topic,
    enum aws_mqtt_qos qos,
    bool retain,
    struct aws_byte_cursor payload,
    aws_mqtt_publish_complete_fn *on_complete,
    void *userdata) {

    assert(connection);

    struct publish_task_arg *arg = aws_mem_acquire(connection->allocator, sizeof(struct publish_task_arg));
    if (!arg) {
        return AWS_OP_ERR;
    }

    arg->connection = connection;
    arg->topic = topic;
    arg->qos = qos;
    arg->retain = retain;
    arg->payload = payload;

    arg->on_complete = on_complete;
    arg->userdata = userdata;

    mqtt_create_request(connection, &s_publish_send, &s_publish_complete, arg);

    return AWS_OP_SUCCESS;
}

static bool s_pingreq_send(uint16_t message_id, bool is_first_attempt, void *userdata) {
    (void)message_id;
    (void)is_first_attempt;

    struct aws_mqtt_client_connection *connection = userdata;

    if (is_first_attempt) {
        /* First attempt, actually send the packet */

        struct aws_mqtt_packet_connection pingreq;
        aws_mqtt_packet_pingreq_init(&pingreq);

        struct aws_io_message *message = mqtt_get_message_for_packet(connection, &pingreq.fixed_header);
        if (!message) {
            goto handle_error;
        }
        struct aws_byte_cursor message_cursor = {
            .ptr = message->message_data.buffer,
            .len = message->message_data.capacity,
        };
        if (aws_mqtt_packet_connection_encode(&message_cursor, &pingreq)) {
            goto handle_error;
        }
        message->message_data.len = message->message_data.capacity - message_cursor.len;

        if (aws_channel_slot_send_message(connection->slot, message, AWS_CHANNEL_DIR_WRITE)) {

            goto handle_error;
        }

        return false;

    handle_error:
        if (message) {
            aws_channel_release_message_to_pool(connection->slot->channel, message);
        }

        return true;
    }

    /* Check that a pingresp has been recieved since pingreq was sent */

    uint64_t current_time = 0;
    aws_channel_current_clock_time(connection->slot->channel, &current_time);

    if (current_time - connection->last_pingresp_timestamp > request_timeout_ns) {
        /* It's been too long since the last ping, close the connection */

        mqtt_disconnect_impl(connection, AWS_ERROR_MQTT_TIMEOUT);
    }

    return true;
}

int aws_mqtt_client_ping(struct aws_mqtt_client_connection *connection) {

    mqtt_create_request(connection, &s_pingreq_send, NULL, connection);

    return AWS_OP_SUCCESS;
}

void aws_mqtt_load_error_strings() {

    static bool s_error_strings_loaded = false;
    if (!s_error_strings_loaded) {

        s_error_strings_loaded = true;

#define AWS_DEFINE_ERROR_INFO_MQTT(C, ES) AWS_DEFINE_ERROR_INFO(C, ES, "libaws-c-mqtt")
        /* clang-format off */
        static struct aws_error_info s_errors[] = {
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_INVALID_RESERVED_BITS,
                "Bits marked as reserved in the MQTT spec were incorrectly set."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_BUFFER_TOO_BIG,
                "[MQTT-1.5.3] Encoded UTF-8 buffers may be no bigger than 65535 bytes."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_INVALID_REMAINING_LENGTH,
                "[MQTT-2.2.3] Encoded remaining length field is malformed."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_UNSUPPORTED_PROTOCOL_NAME,
                "[MQTT-3.1.2-1] Protocol name specified is unsupported."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_UNSUPPORTED_PROTOCOL_LEVEL,
                "[MQTT-3.1.2-2] Protocol level specified is unsupported."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_INVALID_CREDENTIALS,
                "[MQTT-3.1.2-21] Connect packet may not include password when no username is present."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_INVALID_QOS,
                "Both bits in a QoS field must not be set."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_INVALID_PACKET_TYPE,
                "Packet type in packet fixed header is invalid."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_TIMEOUT,
                "Time limit between request and response has been exceeded."),
            AWS_DEFINE_ERROR_INFO_MQTT(
                AWS_ERROR_MQTT_PROTOCOL_ERROR,
                "Protocol error occured."),
        };
        /* clang-format on */
#undef AWS_DEFINE_ERROR_INFO_MQTT

        static struct aws_error_info_list s_list = {
            .error_list = s_errors,
            .count = AWS_ARRAY_SIZE(s_errors),
        };
        aws_register_error_info(&s_list);
    }
}