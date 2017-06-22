#include <avsystem/commons/log.h>
#include <anjay/anjay.h>
#include <anjay/security.h>
#include <anjay/server.h>

#include <poll.h>
#include <string.h>

static int load_buffer_from_file(uint8_t **out, size_t *out_size,
                                 const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        avs_log(tutorial, ERROR, "could not open %s", filename);
        return -1;
    }
    int result = -1;
    if (fseek(f, 0, SEEK_END)) {
        goto finish;
    }
    long size = ftell(f);
    if (size < 0 || (unsigned long) size > SIZE_MAX
            || fseek(f, 0, SEEK_SET)) {
        goto finish;
    }
    *out_size = (size_t) size;
    if (!(*out = (uint8_t *) malloc(*out_size))) {
        goto finish;
    }
    if (fread(*out, *out_size, 1, f) != 1) {
        free(*out);
        *out = NULL;
        goto finish;
    }
    result = 0;
finish:
    fclose(f);
    if (result) {
        avs_log(tutorial, ERROR, "could not read %s", filename);
    }
    return result;
}

static int main_loop(anjay_t *anjay) {
    while (true) {
        // Obtain all network data sources
        AVS_LIST(avs_net_abstract_socket_t *const) sockets =
                anjay_get_sockets(anjay);

        // Prepare to poll() on them
        size_t numsocks = AVS_LIST_SIZE(sockets);
        struct pollfd pollfds[numsocks];
        size_t i = 0;
        AVS_LIST(avs_net_abstract_socket_t *const) sock;
        AVS_LIST_FOREACH(sock, sockets) {
            pollfds[i].fd = *(const int *) avs_net_socket_get_system(*sock);
            pollfds[i].events = POLLIN;
            pollfds[i].revents = 0;
            ++i;
        }

        const int max_wait_time_ms = 1000;
        // Determine the expected time to the next job in milliseconds.
        // If there is no job we will wait till something arrives for
        // at most 1 second (i.e. max_wait_time_ms).
        int wait_ms =
                anjay_sched_calculate_wait_time_ms(anjay, max_wait_time_ms);

        // Wait for the events if necessary, and handle them.
        if (poll(pollfds, numsocks, wait_ms) > 0) {
            int socket_id = 0;
            AVS_LIST(avs_net_abstract_socket_t *const) socket = NULL;
            AVS_LIST_FOREACH(socket, sockets) {
                if (pollfds[socket_id].revents) {
                    if (anjay_serve(anjay, *socket)) {
                        avs_log(tutorial, ERROR, "anjay_serve failed");
                    }
                }
                ++socket_id;
            }
        }

        // Finally run the scheduler (ignoring its return value, which
        // is the amount of tasks executed)
        (void) anjay_sched_run(anjay);
    }
    return 0;
}

static const anjay_dm_object_def_t **create_and_init_security_object(void) {
    bool success = false;

    const anjay_dm_object_def_t **security_obj = anjay_security_object_create();
    if (!security_obj) {
        return NULL;
    }

    anjay_security_instance_t security_instance = {
        .ssid = 1,
        .server_uri = "coaps://localhost:5684",
        .security_mode = ANJAY_UDP_SECURITY_CERTIFICATE
    };

    if (load_buffer_from_file(
                (uint8_t **) &security_instance.public_cert_or_psk_identity,
                &security_instance.public_cert_or_psk_identity_size,
                "client_cert.der")
            || load_buffer_from_file(
                (uint8_t **) &security_instance.private_cert_or_psk_key,
                &security_instance.private_cert_or_psk_key_size,
                "client_key.der")
            || load_buffer_from_file(
                (uint8_t **) &security_instance.server_public_key,
                &security_instance.server_public_key_size,
                "server_cert.der")) {
        goto cleanup;
    }

    anjay_iid_t security_instance_id = ANJAY_IID_INVALID;
    if (!anjay_security_object_add_instance(security_obj, &security_instance,
                                            &security_instance_id)) {
        success = true;
    }
cleanup:
    free((uint8_t *) security_instance.public_cert_or_psk_identity);
    free((uint8_t *) security_instance.private_cert_or_psk_key);
    free((uint8_t *) security_instance.server_public_key);
    if (!success) {
        anjay_security_object_delete(security_obj);
        security_obj = NULL;
    }
    return security_obj;
}

static const anjay_dm_object_def_t **create_and_init_server_object(void) {
    const anjay_dm_object_def_t **server_obj = anjay_server_object_create();
    if (!server_obj) {
        return NULL;
    }

    const anjay_server_instance_t server_instance = {
        .ssid = 1,
        .lifetime = 86400,
        .default_min_period = -1,
        .default_max_period = -1,
        .disable_timeout = -1,
        .binding = ANJAY_BINDING_U
    };

    anjay_iid_t server_instance_id = ANJAY_IID_INVALID;
    if (anjay_server_object_add_instance(server_obj, &server_instance,
                                         &server_instance_id)) {
        anjay_server_object_delete(server_obj);
        return NULL;
    }
    return server_obj;
}

int main(int argc, char *argv[]) {
    static const anjay_configuration_t CONFIG = {
        .endpoint_name = "urn:dev:os:anjay-tutorial",
        .dtls_version = AVS_NET_SSL_VERSION_TLSv1_2,
        .in_buffer_size = 4000,
        .out_buffer_size = 4000
    };

    anjay_t *anjay = anjay_new(&CONFIG);
    if (!anjay) {
        avs_log(tutorial, ERROR, "Could not create Anjay object");
        return -1;
    }
    int result = 0;

    // Instantiate necessary objects
    const anjay_dm_object_def_t **security_obj =
            create_and_init_security_object();
    const anjay_dm_object_def_t **server_obj = create_and_init_server_object();

    // For some reason we were unable to instantiate objects.
    if (!security_obj || !server_obj) {
        result = -1;
        goto cleanup;
    }

    // Register them within Anjay
    if (anjay_register_object(anjay, security_obj)
            || anjay_register_object(anjay, server_obj)) {
        result = -1;
        goto cleanup;
    }

    result = main_loop(anjay);

cleanup:
    anjay_delete(anjay);
    if (security_obj) {
        anjay_security_object_delete(security_obj);
    }
    if (server_obj) {
        anjay_server_object_delete(server_obj);
    }
    return result;
}
