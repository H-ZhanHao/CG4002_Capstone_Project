/**
 * @file tcp_client.c
 * @brief Persistent queued TCP transport with reconnect backoff.
 *
 * This module owns all upstream socket operations. It consumes logical IMU
 * samples from a bounded FreeRTOS queue, encodes them as protocol DATA
 * frames, sends each complete frame over a persistent TCP connection, and
 * incrementally reconstructs bounded downlink frames from nonblocking reads.
 *
 * Repeated connection failures use exponential backoff from one second up
 * to thirty seconds. The delay resets only after a connection remains
 * healthy for five seconds.
 */

#include "tcp_client.h"

#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "app_config.h"
#if APP_TRANSPORT_TLS
#include "esp_tls.h"
#endif
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "imu_source.h"
#include "application.h"
#include "sensor_source.h"
#include "protocol.h"
#include "physical_pipeline.h"

#define BENCHMARK_MAX_BATCH_FRAMES 16

#define TCP_TASK_STACK_SIZE 5120
#define TCP_TASK_PRIORITY   5

#define INITIAL_RETRY_DELAY_MS  1000
#define MAX_RETRY_DELAY_MS      30000
#define BACKOFF_RESET_AFTER_MS  5000
#define CONNECT_TIMEOUT_MS      5000
#define SEND_TIMEOUT_MS          2000
#define HELLO_ACK_TIMEOUT_MS     5000
#define RECEIVE_POLL_DELAY_MS    10
#define SAMPLE_QUEUE_POLL_MS     10
#define HEARTBEAT_INTERVAL_US    1000000

typedef struct {
    uint8_t data[PROTOCOL_MAX_FRAME_SIZE];
    size_t length;
} receive_buffer_t;

typedef struct {
    bool active;
    int64_t last_data_sent_us;
    int64_t last_heartbeat_sent_us;
    uint32_t heartbeat_sequence;
    uint16_t latest_status;
} source_transport_state_t;

typedef struct {
    bool active;
    uint32_t control_sequence;
    uint32_t duration_ms;
    uint16_t batch_frames;
    uint32_t next_sequence;
    int64_t started_us;
    uint32_t frames_sent;
    uint32_t application_bytes_sent;
    uint32_t send_failures;
    uint32_t queue_drops_at_start;
} benchmark_state_t;

static const char *TAG = "tcp_client";

static QueueHandle_t input_queue = NULL;
static TaskHandle_t tcp_task_handle = NULL;
static struct sockaddr_in server_address;
static protocol_hello_t hello_message;
static uint8_t active_node_mask = 0;
static tcp_client_overflow_count_fn_t get_overflow_count = NULL;
static bool tls_enabled = false;
static bool physical_samples_enabled = false;
static const char *tls_ca_certificate = NULL;
static const char *tls_server_identity = NULL;
/* Singleton transport task owns this bounded buffer; keep it off its TLS-heavy stack. */
static uint8_t benchmark_batch_buffer[
    BENCHMARK_MAX_BATCH_FRAMES * PROTOCOL_DATA_PACKET_SIZE];
/* Also keep the queue's maximum-size response off the TLS-heavy task stack. */
static application_response_t transport_application_response;

typedef struct {
    int socket_fd;
#if APP_TRANSPORT_TLS
    esp_tls_t *tls;
#else
    void *tls;
#endif
} transport_connection_t;

/* Complete a nonblocking TCP connect within the fixed connection deadline. */
static bool connect_with_timeout(
    int socket_fd,
    const struct sockaddr *address,
    socklen_t address_length)
{
    int original_flags = fcntl(
        socket_fd,
        F_GETFL,
        0
    );

    if (original_flags < 0) {
        ESP_LOGE(
            TAG,
            "Could not read socket flags; errno=%d",
            errno
        );

        return false;
    }

    if (fcntl(
            socket_fd,
            F_SETFL,
            original_flags | O_NONBLOCK
        ) < 0) {

        ESP_LOGE(
            TAG,
            "Could not make socket nonblocking; errno=%d",
            errno
        );

        return false;
    }

    int result = connect(
        socket_fd,
        address,
        address_length
    );

    if (result != 0 && errno != EINPROGRESS) {
        int connection_error = errno;

        fcntl(socket_fd, F_SETFL, original_flags);
        errno = connection_error;
        return false;
    }

    if (result != 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(socket_fd, &write_set);

        struct timeval timeout = {
            .tv_sec = CONNECT_TIMEOUT_MS / 1000,
            .tv_usec =
                (CONNECT_TIMEOUT_MS % 1000) * 1000,
        };

        result = select(
            socket_fd + 1,
            NULL,
            &write_set,
            NULL,
            &timeout
        );

        if (result == 0) {
            fcntl(socket_fd, F_SETFL, original_flags);
            errno = ETIMEDOUT;
            return false;
        }

        if (result < 0) {
            int connection_error = errno;

            fcntl(socket_fd, F_SETFL, original_flags);
            errno = connection_error;
            return false;
        }

        int socket_error = 0;
        socklen_t error_length = sizeof(socket_error);

        if (getsockopt(
                socket_fd,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_length
            ) != 0) {

            int connection_error = errno;

            fcntl(socket_fd, F_SETFL, original_flags);
            errno = connection_error;
            return false;
        }

        if (socket_error != 0) {
            fcntl(socket_fd, F_SETFL, original_flags);
            errno = socket_error;
            return false;
        }
    }

    if (fcntl(
            socket_fd,
            F_SETFL,
            original_flags
        ) < 0) {

        ESP_LOGE(
            TAG,
            "Could not restore socket flags; errno=%d",
            errno
        );

        return false;
    }

    return true;
}

/* Bound blocking socket operations so failure always returns to reconnect logic. */
static bool configure_socket_timeouts(int socket_fd)
{
    struct timeval send_timeout = {
        .tv_sec = SEND_TIMEOUT_MS / 1000,
        .tv_usec = (SEND_TIMEOUT_MS % 1000) * 1000,
    };

    if (setsockopt(
            socket_fd,
            SOL_SOCKET,
            SO_SNDTIMEO,
            &send_timeout,
            sizeof(send_timeout)
        ) != 0) {

        ESP_LOGE(
            TAG,
            "Could not set send timeout; errno=%d",
            errno
        );

        return false;
    }

    return true;
}

/* Open TCP and, when configured, authenticate the TLS server before use. */
static bool transport_connect(transport_connection_t *connection)
{
    memset(connection, 0, sizeof(*connection));
    connection->socket_fd = -1;

    if (tls_enabled) {
#if APP_TRANSPORT_TLS
        char address_text[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &server_address.sin_addr,
                address_text, sizeof(address_text)) == NULL) return false;

        esp_tls_cfg_t config = {
             /* Trust only certificates signed by the provisioned CA. */
            .cacert_buf = (const unsigned char *)tls_ca_certificate,
            .cacert_bytes = strlen(tls_ca_certificate) + 1,
            .timeout_ms = CONNECT_TIMEOUT_MS,

            /* Verify that the certificate belongs to the expected laptop relay. */
            .common_name = tls_server_identity,
            .skip_common_name = false,
            .non_block = true,
        };
        connection->tls = esp_tls_init();
        if (connection->tls == NULL) return false;

        int result = esp_tls_conn_new_sync(
            address_text,
            strlen(address_text),
            ntohs(server_address.sin_port),
            &config,
            connection->tls
        );
        if (result != 1) {
            ESP_LOGE(TAG, "TLS connection or certificate verification failed");
            esp_tls_conn_destroy(connection->tls);
            connection->tls = NULL;
            return false;
        }
        return true;
#else
        ESP_LOGE(TAG, "TLS requested by a non-TLS build");
        return false;
#endif
    }

    connection->socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (connection->socket_fd < 0) return false;
    if (!connect_with_timeout(connection->socket_fd,
            (struct sockaddr *)&server_address, sizeof(server_address)) ||
        !configure_socket_timeouts(connection->socket_fd)) {
        close(connection->socket_fd);
        connection->socket_fd = -1;
        return false;
    }
    return true;
}

/* Read through the active plain-TCP or TLS backend behind one interface. */
static int transport_read(
    transport_connection_t *connection, uint8_t *data, size_t data_length)
{
    if (connection->tls != NULL) {
#if APP_TRANSPORT_TLS
        int result = esp_tls_conn_read(connection->tls, data, data_length);
        if (result == ESP_TLS_ERR_SSL_WANT_READ ||
            result == ESP_TLS_ERR_SSL_WANT_WRITE) return -2;
        return result;
#else
        return -1;
#endif
    }
    int result = recv(connection->socket_fd, data, data_length, MSG_DONTWAIT);
    if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -2;
    return result;
}

/* Write through the active plain-TCP or TLS backend behind one interface. */
static int transport_write(
    transport_connection_t *connection, const uint8_t *data, size_t data_length)
{
    if (connection->tls != NULL) {
#if APP_TRANSPORT_TLS
        int result = esp_tls_conn_write(connection->tls, data, data_length);
        if (result == ESP_TLS_ERR_SSL_WANT_READ ||
            result == ESP_TLS_ERR_SSL_WANT_WRITE) return -2;
        return result;
#else
        return -1;
#endif
    }
    return send(connection->socket_fd, data, data_length, 0);
}

/* Release TLS and socket state so the next retry begins from a clean boundary. */
static void transport_close(transport_connection_t *connection)
{
    if (connection->tls != NULL) {
#if APP_TRANSPORT_TLS
        esp_tls_conn_destroy(connection->tls);
#endif
        connection->tls = NULL;
    }
    if (connection->socket_fd >= 0) {
        shutdown(connection->socket_fd, SHUT_RDWR);
        close(connection->socket_fd);
        connection->socket_fd = -1;
    }
}

/* Retry partial writes until one encoded frame is completely transmitted. */
static bool send_complete(
    transport_connection_t *connection,
    const uint8_t *data,
    size_t data_length)
{
    size_t total_sent = 0;

    while (total_sent < data_length) {
        int bytes_sent = transport_write(
            connection,
            data + total_sent,
            data_length - total_sent
        );

        if (bytes_sent == -2) {
            vTaskDelay(pdMS_TO_TICKS(RECEIVE_POLL_DELAY_MS));
            continue;
        }

        if (bytes_sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            ESP_LOGE(
                TAG,
                "TCP send failed; errno=%d",
                errno
            );

            return false;
        }

        if (bytes_sent == 0) {
            ESP_LOGW(
                TAG,
                "TCP connection closed during send"
            );

            return false;
        }

        total_sent += (size_t)bytes_sent;
    }

    return true;
}

/* Stop benchmark generation and send its final counters as one report. */
static bool finish_benchmark(
    transport_connection_t *connection, uint32_t session_id,
    benchmark_state_t *benchmark)
{
    if (!benchmark->active) return true;
    int64_t now_us = esp_timer_get_time();
    protocol_benchmark_report_t report = {
        .control_sequence = benchmark->control_sequence,
        .elapsed_ms = (uint32_t)((now_us - benchmark->started_us) / 1000),
        .frames_sent = benchmark->frames_sent,
        .application_bytes_sent = benchmark->application_bytes_sent,
        .queue_drops = sensor_source_get_dropped_count() -
            benchmark->queue_drops_at_start,
        .send_failures = benchmark->send_failures,
    };
    uint8_t packet[PROTOCOL_BENCHMARK_REPORT_PACKET_SIZE];
    protocol_encode_benchmark_report(packet, hello_message.hub_node_id,
        session_id, benchmark->control_sequence, (uint64_t)now_us, &report);
    benchmark->active = false;
    (void)sensor_source_set_paused(hello_message.hub_node_id, false);
    ESP_LOGI(TAG, "Benchmark stopped: frames=%lu bytes=%lu elapsed=%lums",
        (unsigned long)report.frames_sent,
        (unsigned long)report.application_bytes_sent,
        (unsigned long)report.elapsed_ms);
    return send_complete(connection, packet, sizeof(packet));
}

/* Apply common downlink limits before allocating or dispatching a complete frame. */
static bool validate_received_header(const protocol_header_t *header)
{
    if (header->magic != PROTOCOL_MAGIC) {
        ESP_LOGE(TAG, "Invalid received magic: 0x%04X", header->magic);
        return false;
    }

    if (header->version != PROTOCOL_VERSION) {
        ESP_LOGE(
            TAG,
            "Unsupported received protocol version: %u",
            header->version
        );
        return false;
    }

    if (header->payload_length > PROTOCOL_MAX_PAYLOAD_SIZE) {
        ESP_LOGE(
            TAG,
            "Received payload is too large: %u",
            header->payload_length
        );
        return false;
    }

    return true;
}

/* Consume one parsed frame while retaining any following coalesced TCP bytes. */
static void remove_received_frame(
    receive_buffer_t *receive_buffer,
    size_t frame_length)
{
    size_t remaining_length = receive_buffer->length - frame_length;

    if (remaining_length > 0) {
        memmove(
            receive_buffer->data,
            receive_buffer->data + frame_length,
            remaining_length
        );
    }

    receive_buffer->length = remaining_length;
}

/* Decode a hub-addressed ERROR for readable diagnosis without ending the Session. */
static void log_received_error(
    const receive_buffer_t *receive_buffer,
    const protocol_header_t *header)
{
    protocol_error_t error;

    if (header->node_id != 0 ||
        !protocol_decode_error_payload(
            receive_buffer->data + PROTOCOL_HEADER_SIZE,
            header->payload_length,
            &error)) {

        ESP_LOGE(TAG, "Received malformed ERROR from Ultra96");
        return;
    }

    ESP_LOGE(
        TAG,
        "Received ERROR: code=%u offending_type=0x%02X",
        error.error_code,
        error.offending_message_type
    );
}

/* Complete the session-zero HELLO exchange and obtain the assigned Session ID. */
static bool wait_for_hello_ack(
    transport_connection_t *connection,
    receive_buffer_t *receive_buffer,
    uint32_t *session_id)
{
    int64_t deadline_us = esp_timer_get_time() +
        ((int64_t)HELLO_ACK_TIMEOUT_MS * 1000);

    while (esp_timer_get_time() < deadline_us) {
        while (receive_buffer->length >= PROTOCOL_HEADER_SIZE) {
            protocol_header_t header;

            if (!protocol_decode_header(
                    receive_buffer->data,
                    receive_buffer->length,
                    &header) ||
                !validate_received_header(&header)) {

                return false;
            }

            size_t frame_length =
                PROTOCOL_HEADER_SIZE + header.payload_length;

            if (receive_buffer->length < frame_length) {
                break;
            }

            if (header.message_type == PROTOCOL_MESSAGE_TYPE_ERROR) {
                log_received_error(receive_buffer, &header);
                return false;
            }

            if (header.message_type != PROTOCOL_MESSAGE_TYPE_HELLO_ACK ||
                header.node_id != 0 ||
                header.payload_length != PROTOCOL_HELLO_ACK_PAYLOAD_SIZE ||
                header.session_id == 0) {

                ESP_LOGE(
                    TAG,
                    "Invalid HELLO_ACK: type=0x%02X node=%u "
                    "payload=%u session=%lu",
                    header.message_type,
                    header.node_id,
                    header.payload_length,
                    (unsigned long)header.session_id
                );
                return false;
            }

            *session_id = header.session_id;
            remove_received_frame(receive_buffer, frame_length);

            ESP_LOGI(
                TAG,
                "HELLO accepted; session=%lu",
                (unsigned long)*session_id
            );
            return true;
        }

        if (receive_buffer->length == sizeof(receive_buffer->data)) {
            ESP_LOGE(TAG, "TCP receive buffer is full during handshake");
            return false;
        }

        int bytes_received = transport_read(
            connection,
            receive_buffer->data + receive_buffer->length,
            sizeof(receive_buffer->data) - receive_buffer->length
        );

        if (bytes_received > 0) {
            receive_buffer->length += (size_t)bytes_received;
            continue;
        }

        if (bytes_received == 0) {
            ESP_LOGW(TAG, "TCP peer closed during handshake");
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (bytes_received != -2 && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGE(TAG, "HELLO_ACK receive failed; errno=%d", errno);
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(RECEIVE_POLL_DELAY_MS));
    }

    ESP_LOGE(TAG, "Timed out waiting for HELLO_ACK");
    return false;
}

/* Dispatch every complete frame currently reconstructed in the receive buffer. */
static bool process_received_frames(
    receive_buffer_t *receive_buffer,
    uint32_t session_id)
{
    while (receive_buffer->length >= PROTOCOL_HEADER_SIZE) {
        protocol_header_t header;

        if (!protocol_decode_header(
                receive_buffer->data,
                receive_buffer->length,
                &header)) {

            ESP_LOGE(TAG, "Could not decode received header");
            return false;
        }

        if (!validate_received_header(&header)) {
            return false;
        }

        size_t frame_length =
            PROTOCOL_HEADER_SIZE + header.payload_length;

        if (receive_buffer->length < frame_length) {
            return true;
        }

        if (header.session_id != session_id) {
            ESP_LOGE(
                TAG,
                "Received invalid Session ID: expected=%lu received=%lu",
                (unsigned long)session_id,
                (unsigned long)header.session_id
            );
            return false;
        }

        if (!application_submit_frame(
                session_id,
                receive_buffer->data,
                frame_length)) {
            ESP_LOGE(TAG, "Application command queue is full");
            return false;
        }

        remove_received_frame(receive_buffer, frame_length);
    }

    return true;
}

/* Append currently available stream bytes without blocking the sample send path. */
static bool receive_available(
    transport_connection_t *connection,
    receive_buffer_t *receive_buffer,
    uint32_t session_id)
{
    while (true) {
        if (!process_received_frames(receive_buffer, session_id)) {
            return false;
        }

        if (receive_buffer->length == sizeof(receive_buffer->data)) {
            ESP_LOGE(TAG, "TCP receive buffer is full");
            return false;
        }

        int bytes_received = transport_read(
            connection,
            receive_buffer->data + receive_buffer->length,
            sizeof(receive_buffer->data) - receive_buffer->length
        );

        if (bytes_received > 0) {
            receive_buffer->length += (size_t)bytes_received;
            continue;
        }

        if (bytes_received == 0) {
            ESP_LOGW(TAG, "TCP peer closed the connection");
            return false;
        }

        if (errno == EINTR) {
            continue;
        }

        if (bytes_received == -2 || errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }

        ESP_LOGE(
            TAG,
            "TCP receive failed; errno=%d",
            errno
        );
        return false;
    }
}

/* Initialize heartbeat state only for nodes enabled by the active-node mask. */
static void initialize_source_states(
    source_transport_state_t states[7],
    int64_t session_started_us)
{
    memset(states, 0, sizeof(source_transport_state_t) * 7);

    for (uint8_t node_id = 1; node_id <= 6; node_id++) {
        states[node_id].active =
            (active_node_mask & (1U << node_id)) != 0;
        states[node_id].last_data_sent_us = session_started_us;
        states[node_id].last_heartbeat_sent_us = session_started_us;
    }
}

/* Retain the latest source status used if that source later needs a heartbeat. */
static void record_data_sent(
    source_transport_state_t states[7],
    const imu_sample_t *sample,
    int64_t sent_at_us)
{
    if (sample->node_id < 1 || sample->node_id > 6) {
        return;
    }

    source_transport_state_t *state = &states[sample->node_id];
    state->last_data_sent_us = sent_at_us;
    state->latest_status = sample->status;
}

static void record_physical_data_sent(
    source_transport_state_t states[7],
    const protocol_physical_sample_t *sample, int64_t sent_at_us)
{
    if (sample->node_id < 1 || sample->node_id > 6) return;
    source_transport_state_t *state = &states[sample->node_id];
    state->last_data_sent_us = sent_at_us;
    state->latest_status = sample->status;
}

/* Report active sources that have produced no DATA for the heartbeat interval. */
static bool send_due_heartbeats(
    transport_connection_t *connection,
    uint32_t session_id,
    source_transport_state_t states[7])
{
    int64_t now_us = esp_timer_get_time();

    for (uint8_t node_id = 1; node_id <= 6; node_id++) {
        source_transport_state_t *state = &states[node_id];

        if (!state->active ||
            (now_us - state->last_data_sent_us) < HEARTBEAT_INTERVAL_US ||
            (now_us - state->last_heartbeat_sent_us) <
                HEARTBEAT_INTERVAL_US) {

            continue;
        }

        protocol_heartbeat_t heartbeat = {
            .node_id = node_id,
            .flags = 0,
            .sequence = state->heartbeat_sequence,
            .timestamp_us = (uint64_t)now_us,
            /* Physical nodes intentionally retain samples without publishing
             * before START_FLUSH. That quiet period is not sensor failure. */
            .status = physical_samples_enabled
                ? state->latest_status
                : state->latest_status | PROTOCOL_STATUS_SOURCE_UNAVAILABLE,
            .overflow_count = get_overflow_count == NULL
                ? 0
                : get_overflow_count(node_id),
        };
        uint8_t packet[PROTOCOL_HEARTBEAT_PACKET_SIZE];

        protocol_encode_heartbeat(packet, &heartbeat, session_id);

        if (!send_complete(connection, packet, sizeof(packet))) {
            return false;
        }

        ESP_LOGI(
            TAG,
            "Sent HEARTBEAT: node=%u sequence=%lu overflow=%lu",
            node_id,
            (unsigned long)state->heartbeat_sequence,
            (unsigned long)heartbeat.overflow_count
        );

        state->heartbeat_sequence++;
        state->last_heartbeat_sent_us = now_us;
    }

    return true;
}

/* Double reconnect delay up to the configured maximum. */
static uint32_t next_retry_delay(uint32_t current_delay_ms)
{
    if (current_delay_ms >= MAX_RETRY_DELAY_MS) {
        return MAX_RETRY_DELAY_MS;
    }

    uint32_t doubled_delay = current_delay_ms * 2;

    if (doubled_delay > MAX_RETRY_DELAY_MS) {
        return MAX_RETRY_DELAY_MS;
    }

    return doubled_delay;
}

/* Sleep for the current backoff and prepare the following retry delay. */
static void wait_before_retry(uint32_t *retry_delay_ms)
{
    ESP_LOGW(
        TAG,
        "Reconnecting in %lu ms",
        (unsigned long)*retry_delay_ms
    );

    vTaskDelay(pdMS_TO_TICKS(*retry_delay_ms));

    *retry_delay_ms = next_retry_delay(*retry_delay_ms);
}

/* Own connection, handshake, bidirectional traffic, benchmark, and reconnect state. */
static void tcp_client_task(void *argument)
{
    (void)argument;

    uint32_t retry_delay_ms = INITIAL_RETRY_DELAY_MS;

    while (true) {
        char address_text[INET_ADDRSTRLEN];

        if (inet_ntop(
                AF_INET,
                &server_address.sin_addr,
                address_text,
                sizeof(address_text)
            ) == NULL) {

            ESP_LOGE(TAG, "Could not format server address");
            wait_before_retry(&retry_delay_ms);
            continue;
        }

        ESP_LOGI(
            TAG,
            "Connecting to %s:%u...",
            address_text,
            ntohs(server_address.sin_port)
        );

        transport_connection_t connection;
        bool connected = transport_connect(&connection);

        if (!connected) {
            ESP_LOGE(
                TAG,
                "%s connection failed; errno=%d",
                tls_enabled ? "TLS" : "TCP",
                errno
            );

            wait_before_retry(&retry_delay_ms);
            continue;
        }

        ESP_LOGI(TAG, "%s connection established",
            tls_enabled ? "TLS" : "TCP");

        uint8_t hello_packet[PROTOCOL_HELLO_PACKET_SIZE];
        uint8_t packet[PROTOCOL_PHYSICAL_DATA_PACKET_SIZE];
        receive_buffer_t receive_buffer = {0};
        uint32_t session_id = 0;

        protocol_encode_hello(
            hello_packet,
            &hello_message,
            (uint64_t)esp_timer_get_time()
        );

        ESP_LOGI(
            TAG,
            "Sending HELLO: node=%u boot_id=%lu assigned_mask=0x%02X",
            hello_message.hub_node_id,
            (unsigned long)hello_message.boot_id,
            hello_message.assigned_node_mask
        );

        bool connection_ok = send_complete(
            &connection,
            hello_packet,
            sizeof(hello_packet)
        );

        if (connection_ok) {
            connection_ok = wait_for_hello_ack(
                &connection,
                &receive_buffer,
                &session_id
            );
        }

        if (!connection_ok) {
            transport_close(&connection);
            ESP_LOGW(TAG, "Application handshake failed");
            wait_before_retry(&retry_delay_ms);
            continue;
        }

        int64_t connected_since_us = esp_timer_get_time();
        bool backoff_reset = false;
        source_transport_state_t source_states[7];
        benchmark_state_t benchmark = {0};

        initialize_source_states(source_states, connected_since_us);

#if !APP_PHYSICAL_INTEGRATION
        (void)sensor_source_stop(hello_message.hub_node_id);
#endif
        xQueueReset(input_queue);

        if (!application_begin_session(session_id, hello_message.boot_id)) {
            ESP_LOGE(TAG, "Could not begin application session");
            connection_ok = false;
        }

        while (connection_ok) {
            while (application_receive_response(
                    &transport_application_response,
                    0)) {
                application_response_t *application_response =
                    &transport_application_response;
                if (application_response->session_id != session_id) {
                    ESP_LOGW(TAG, "Discarded stale application response");
                    continue;
                }

                if (application_response->type ==
                    APPLICATION_RESPONSE_DISCONNECT) {
                    ESP_LOGW(TAG, "Application requested disconnect");
                    connection_ok = false;
                    break;
                }

                if (application_response->type == APPLICATION_RESPONSE_TEST) {
                    uint8_t response_packet[PROTOCOL_TEST_PACKET_SIZE];
                    protocol_encode_test_response(
                        response_packet,
                        &application_response->sample,
                        session_id
                    );
                    connection_ok = send_complete(
                        &connection,
                        response_packet,
                        sizeof(response_packet)
                    );
                    continue;
                }

                if (application_response->type == APPLICATION_RESPONSE_ERROR) {
                    uint8_t error_packet[PROTOCOL_ERROR_PACKET_SIZE];
                    protocol_encode_error(
                        error_packet,
                        hello_message.hub_node_id,
                        session_id,
                        application_response->sample.sequence,
                        (uint64_t)esp_timer_get_time(),
                        application_response->error_code,
                        application_response->offending_message_type
                    );
                    connection_ok = send_complete(
                        &connection,
                        error_packet,
                        sizeof(error_packet)
                    );
                    continue;
                }

                if (application_response->type == APPLICATION_RESPONSE_SYNC) {
                    uint8_t sync_packet[PROTOCOL_SYNC_RESP_PACKET_SIZE];
                    protocol_sync_response_t sync_response =
                        application_response->sync_response;
                    /* t3 must be captured next to the actual socket write, not
                     * when the application task first queues the response. */
                    sync_response.node_send_us =
                        (uint64_t)esp_timer_get_time();
                    protocol_encode_sync_response(
                        sync_packet,
                        hello_message.hub_node_id,
                        session_id,
                        application_response->sync_sequence,
                        &sync_response
                    );
                    connection_ok = send_complete(
                        &connection, sync_packet, sizeof(sync_packet));
                    continue;
                }

                if (application_response->type ==
                    APPLICATION_RESPONSE_CONTROL_ACK) {
                    uint8_t ack_packet[PROTOCOL_CONTROL_ACK_PACKET_SIZE];
                    uint64_t now_us = (uint64_t)esp_timer_get_time();
                    protocol_encode_control_ack(
                        ack_packet,
                        hello_message.hub_node_id,
                        session_id,
                        application_response->control_ack_response_sequence,
                        now_us,
                        &application_response->control_ack
                    );
                    connection_ok = send_complete(
                        &connection, ack_packet, sizeof(ack_packet));
                    continue;
                }

                if (application_response->type ==
                    APPLICATION_RESPONSE_ACTIVITY_TRIGGER) {
                    uint8_t trigger_packet[PROTOCOL_ACTIVITY_TRIGGER_PACKET_SIZE];
                    const physical_trigger_event_t *event =
                        &application_response->trigger_event;
                    protocol_encode_activity_trigger(
                        trigger_packet, hello_message.hub_node_id, session_id,
                        event->sequence, event->detected_timestamp_us,
                        &event->trigger);
                    connection_ok = send_complete(
                        &connection, trigger_packet, sizeof(trigger_packet));
                    continue;
                }

                if (application_response->type == APPLICATION_RESPONSE_FILE) {
                    if (application_response->file_frame_length != 0) {
                        connection_ok = send_complete(&connection,
                            application_response->file_frame,
                            application_response->file_frame_length);
                    }
                    if (connection_ok && application_response->file_has_result) {
                        uint8_t result_packet[PROTOCOL_FILE_RESULT_PACKET_SIZE];
                        protocol_encode_file_result(result_packet,
                            hello_message.hub_node_id, session_id,
                            application_response->file_sequence,
                            (uint64_t)esp_timer_get_time(),
                            &application_response->file_result);
                        connection_ok = send_complete(&connection, result_packet,
                            sizeof(result_packet));
                    }
                    continue;
                }

                if (application_response->type ==
                    APPLICATION_RESPONSE_BENCHMARK_CONTROL) {
                    protocol_benchmark_control_t control =
                        application_response->benchmark_control;
                    if (control.action == PROTOCOL_BENCHMARK_ACTION_START) {
                        if (benchmark.active) {
                            connection_ok = finish_benchmark(
                                &connection, session_id, &benchmark);
                        }
                        if (!connection_ok) break;
                        benchmark.active = true;
                        benchmark.control_sequence =
                            application_response->control_sequence;
                        benchmark.duration_ms = control.duration_ms;
                        benchmark.batch_frames = control.batch_frames >
                            BENCHMARK_MAX_BATCH_FRAMES
                            ? BENCHMARK_MAX_BATCH_FRAMES
                            : control.batch_frames;
                        benchmark.next_sequence = 0;
                        benchmark.frames_sent = 0;
                        benchmark.application_bytes_sent = 0;
                        benchmark.send_failures = 0;
                        benchmark.started_us = esp_timer_get_time();
                        benchmark.queue_drops_at_start =
                            sensor_source_get_dropped_count();
                        (void)sensor_source_set_paused(
                            hello_message.hub_node_id, true);
                        xQueueReset(input_queue);
                        ESP_LOGI(TAG,
                            "Benchmark started: duration=%lums batch=%u",
                            (unsigned long)benchmark.duration_ms,
                            benchmark.batch_frames);
                    } else {
                        connection_ok = finish_benchmark(
                            &connection, session_id, &benchmark);
                    }
                }
            }

            if (!connection_ok) {
                break;
            }

            if (benchmark.active) {
                size_t batch_length = 0;
                for (uint16_t i = 0; i < benchmark.batch_frames; ++i) {
                    uint32_t sequence = benchmark.next_sequence++;
                    imu_sample_t benchmark_sample = {
                        .node_id = hello_message.hub_node_id,
                        .sequence = sequence,
                        .timestamp_us = (uint64_t)esp_timer_get_time(),
                        .ax = (int16_t)(sequence & 0x7FFF),
                        .ay = (int16_t)(-(int32_t)(sequence & 0x7FFF)),
                        .az = 16384,
                        .gx = (int16_t)(sequence % 1000),
                        .gy = (int16_t)-(int32_t)(sequence % 1000),
                        .status = PROTOCOL_STATUS_MOCK_DATA |
                            PROTOCOL_STATUS_BENCHMARK,
                    };
                    protocol_encode_data(&benchmark_batch_buffer[batch_length],
                        &benchmark_sample, session_id);
                    batch_length += PROTOCOL_DATA_PACKET_SIZE;
                }
                connection_ok = send_complete(
                    &connection, benchmark_batch_buffer, batch_length);
                if (connection_ok) {
                    benchmark.frames_sent += benchmark.batch_frames;
                    benchmark.application_bytes_sent += (uint32_t)batch_length;
                } else {
                    benchmark.send_failures++;
                }
            } else {
            if (physical_samples_enabled) {
                protocol_physical_sample_t sample;
                bool available = xQueueReceive(input_queue, &sample,
                    pdMS_TO_TICKS(SAMPLE_QUEUE_POLL_MS)) == pdPASS;
                if (available) {
                    connection_ok = protocol_encode_physical_data(
                        packet, &sample, session_id) &&
                        send_complete(&connection, packet,
                            PROTOCOL_PHYSICAL_DATA_PACKET_SIZE);
                    if (connection_ok) record_physical_data_sent(
                        source_states, &sample, esp_timer_get_time());
                    if (connection_ok && (sample.sequence % 50) == 0) {
                        ESP_LOGI(TAG,
                            "Sent PHYSICAL_DATA node=%u sequence=%lu queue=%u",
                            sample.node_id, (unsigned long)sample.sequence,
                            (unsigned int)uxQueueMessagesWaiting(input_queue));
                    }
                }
            } else {
                imu_sample_t sample;
                bool sample_available = xQueueReceive(
                    input_queue, &sample,
                    pdMS_TO_TICKS(SAMPLE_QUEUE_POLL_MS)) == pdPASS;
                if (sample_available) {
                    protocol_encode_data(packet, &sample, session_id);
                    connection_ok = send_complete(
                        &connection, packet, PROTOCOL_DATA_PACKET_SIZE);
                    if (connection_ok) record_data_sent(
                        source_states, &sample, esp_timer_get_time());
                    if (connection_ok && (sample.sequence % 50) == 0) {
                    ESP_LOGI(
                        TAG,
                        "Sent: node=%u sequence=%lu "
                        "timestamp_us=%llu queue=%u heap=%lu min_heap=%lu",
                        sample.node_id,
                        (unsigned long)sample.sequence,
                        (unsigned long long)sample.timestamp_us,
                        (unsigned int)uxQueueMessagesWaiting(input_queue),
                        (unsigned long)esp_get_free_heap_size(),
                        (unsigned long)esp_get_minimum_free_heap_size()
                    );
                }
                }
            }
            }

            if (connection_ok && !benchmark.active) {
                connection_ok = send_due_heartbeats(
                    &connection,
                    session_id,
                    source_states
                );
            }

            if (connection_ok) {
                connection_ok = receive_available(
                    &connection,
                    &receive_buffer,
                    session_id
                );
            }

            if (connection_ok && benchmark.active &&
                esp_timer_get_time() - benchmark.started_us >=
                    (int64_t)benchmark.duration_ms * 1000) {
                connection_ok = finish_benchmark(
                    &connection, session_id, &benchmark);
            }

            if (connection_ok && !backoff_reset) {
                int64_t connected_duration_us =
                    esp_timer_get_time() - connected_since_us;

                if (connected_duration_us >=
                    ((int64_t)BACKOFF_RESET_AFTER_MS * 1000)) {

                    retry_delay_ms = INITIAL_RETRY_DELAY_MS;
                    backoff_reset = true;

                    ESP_LOGI(
                        TAG,
                        "Connection stable for %d ms; "
                        "reconnect backoff reset",
                        BACKOFF_RESET_AFTER_MS
                    );
                }
            }
        }

        if (benchmark.active) {
            benchmark.active = false;
        }
#if !APP_PHYSICAL_INTEGRATION
        (void)sensor_source_stop(hello_message.hub_node_id);
#endif
        xQueueReset(input_queue);
        transport_close(&connection);

        application_end_session(session_id);

        ESP_LOGW(TAG, "%s connection lost", tls_enabled ? "TLS" : "TCP");
        wait_before_retry(&retry_delay_ms);
    }
}

/* Validate and copy configuration before launching the singleton transport task. */
esp_err_t tcp_client_start(const tcp_client_config_t *config)
{
    if (config == NULL ||
        config->sample_queue == NULL ||
        config->server_ip == NULL ||
        config->server_port == 0 ||
        config->hub_node_id < 1 ||
        config->hub_node_id > 6 ||
        config->assigned_node_mask == 0 ||
        config->active_node_mask == 0 ||
        (config->active_node_mask & ~config->assigned_node_mask) != 0 ||
        config->device_token == NULL ||
        (config->use_tls &&
            (config->tls_ca_certificate == NULL ||
             config->tls_server_identity == NULL))) {

        return ESP_ERR_INVALID_ARG;
    }

    if (tcp_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&server_address, 0, sizeof(server_address));
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(config->server_port);

    if (inet_pton(
            AF_INET,
            config->server_ip,
            &server_address.sin_addr
        ) != 1) {

        return ESP_ERR_INVALID_ARG;
    }

    input_queue = config->sample_queue;
    active_node_mask = config->active_node_mask;
    get_overflow_count = config->get_overflow_count;
    tls_enabled = config->use_tls;
    physical_samples_enabled = config->physical_samples;
    tls_ca_certificate = config->tls_ca_certificate;
    tls_server_identity = config->tls_server_identity;

    memset(&hello_message, 0, sizeof(hello_message));
    hello_message.hub_node_id = config->hub_node_id;
    hello_message.firmware_major = config->firmware_major;
    hello_message.firmware_minor = config->firmware_minor;
    hello_message.firmware_patch = config->firmware_patch;
    hello_message.supported_sampling_rates =
        config->supported_sampling_rates;
    hello_message.assigned_node_mask = config->assigned_node_mask;
    memcpy(
        hello_message.device_token,
        config->device_token,
        PROTOCOL_DEVICE_TOKEN_SIZE
    );

    do {
        hello_message.boot_id = esp_random();
    } while (hello_message.boot_id == 0);

    BaseType_t result = xTaskCreate(
        tcp_client_task,
        "tcp_client",
        TCP_TASK_STACK_SIZE,
        NULL,
        TCP_TASK_PRIORITY,
        &tcp_task_handle
    );

    if (result != pdPASS) {
        input_queue = NULL;
        active_node_mask = 0;
        get_overflow_count = NULL;
        tcp_task_handle = NULL;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}
