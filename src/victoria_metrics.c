/*
 * victoria_metrics.c
 *
 *  Created on: May 4, 2023
 *      Author: Edvard
 */

#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

#include <slash/slash.h>
#include <slash/optparse.h>

#include <csp/csp.h>
#include <param/param_queue.h>
#include "param_sniffer.h"

static pthread_t vm_push_thread;
int vm_running = 0;

#define SERVER_PORT      8428
#define SERVER_PORT_AUTH 8427
#define BUFFER_SIZE      10 * 1024 * 1024

char buffer[BUFFER_SIZE];
size_t buffer_size = 0;
pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    int use_ssl;
    int port;
    int skip_verify;
    int verbose;
    char * username;
    char * password;
    char * server_ip;
} vm_args;

size_t write_callback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    return size * nmemb;
}

void * vm_push(void * arg) {

    vm_args * args = arg;

    CURL * curl;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_ALL);
    struct curl_slist * headers = NULL;

    curl = curl_easy_init();
    const char * hostname = csp_get_conf()->hostname;
    char url[256];
    char protocol[8] = "http";

    if (args->use_ssl) {
        protocol[4] = 's';
    }

    if (curl) {
        if (args->skip_verify) {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }
        if (args->verbose) {
            curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
        }else
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);


        if (args->username && args->password) {
            curl_easy_setopt(curl, CURLOPT_USERNAME, args->username);
            curl_easy_setopt(curl, CURLOPT_PASSWORD, args->password);
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        }

        // Test connection
        snprintf(url, sizeof(url), "%s://%s:%d/prometheus/api/v1/query", protocol, args->server_ip, args->port);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "query=test42");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 12);
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Failed test of connection: %s\n", curl_easy_strerror(res));
            vm_running = 0;
        }
        long response_code;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code != 200 && res == CURLE_OK) {
            printf("Failed test with response code: %ld\n", response_code);
            vm_running = 0;
        }

        // Resume building of header for push
        snprintf(url, sizeof(url), "%s://%s:%d/api/v1/import/prometheus?extra_label=instance=%s", protocol, args->server_ip, args->port, hostname);
        curl_easy_setopt(curl, CURLOPT_URL, url);
        headers = curl_slist_append(headers, "Content-Type: text/plain");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        if (args->verbose) {
            printf("Full URL: %s\n", url);
        }

    } else {
        vm_running = 0;
        printf("curl_easy_init() failed\n");
    }

    if (vm_running) {
        printf("Connection established to %s://%s:%d\n", protocol, args->server_ip, args->port);
    }

    while (vm_running) {
        // Lock the buffer mutex
        pthread_mutex_lock(&buffer_mutex);
        if (buffer_size == 0) {
            pthread_mutex_unlock(&buffer_mutex);
            sleep(1);
            continue;
        }

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buffer);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, buffer_size);
        res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            printf("Failed push: %s", curl_easy_strerror(res));
        }

        // Unlock the buffer mutex
        pthread_mutex_unlock(&buffer_mutex);

        pthread_mutex_lock(&buffer_mutex);
        buffer_size = 0;
        pthread_mutex_unlock(&buffer_mutex);

        sleep(1);
    }

    printf("vm push stopped\n");
    // Clean up
    if (curl) {
        curl_easy_cleanup(curl);
    }
    if (headers) {
        curl_slist_free_all(headers);
    }
    curl_global_cleanup();
    if (args->username) {
        free(args->username);
        args->username = NULL;
    }
    if (args->password) {
        free(args->password);
        args->password = NULL;
    }
    if (args->server_ip) {
        free(args->server_ip);
        args->server_ip = NULL;
    }
    free(args);
    return NULL;
}

void vm_add(char * metric_line) {

    // Lock the buffer mutex
    pthread_mutex_lock(&buffer_mutex);

    // Check if there's enough space in the buffer
    size_t line_len = strlen(metric_line);
    if (buffer_size + line_len < BUFFER_SIZE) {
        // Add the new metric line to the buffer
        strncpy(buffer + buffer_size, metric_line, BUFFER_SIZE - buffer_size);
        buffer_size += line_len;
    }

    // Unlock the buffer mutex
    pthread_mutex_unlock(&buffer_mutex);
}

static int vm_start_cmd(struct slash * slash) {

    if (vm_running) return SLASH_SUCCESS;

    int hk_node = 0;
    int logfile = 0;
    char * tmp_username = NULL;
    char * tmp_password = NULL;
    vm_args * args = calloc(1, sizeof(vm_args));

    optparse_t * parser = optparse_new("vm start", "<server>");
    optparse_add_help(parser);
    optparse_add_string(parser, 'u', "user", "STRING", &tmp_username, "Username for vmauth");
    optparse_add_string(parser, 'p', "pass", "STRING", &tmp_password, "Password for vmauth");
    optparse_add_set(parser, 's', "ssl", 1, &(args->use_ssl), "Use SSL/TLS");
    optparse_add_int(parser, 'P', "server-port", "NUM", 0, &(args->port), "Overwrite default port");
    optparse_add_int(parser, 'n', "hk_node", "NUM", 0, &hk_node, "Housekeeping node");
    optparse_add_set(parser, 'l', "logfile", 1, &logfile, "Enable logging to param_sniffer.log");
    optparse_add_set(parser, 'S', "skip-verify", 1, &(args->skip_verify), "Skip verification of the server's cert and hostname");
    optparse_add_set(parser, 'v', "verbose", 1, &(args->verbose), "Verbose connect");

    int argi = optparse_parse(parser, slash->argc - 1, (const char **)slash->argv + 1);

    if (argi < 0) {
        optparse_del(parser);
        return SLASH_EINVAL;
    }

    if (++argi >= slash->argc) {
        printf("Missing server ip/domain\n");
        optparse_del(parser);
        return SLASH_EINVAL;
    }

    if (tmp_username) {
        if (!tmp_password) {
            printf("Provide password with -p\n");
            optparse_del(parser);
            return SLASH_EINVAL;
        }
        args->username = strdup(tmp_username);
        args->password = strdup(tmp_password);
        if (!args->port) {
            args->port = SERVER_PORT_AUTH;
        }
    } else if (!args->port) {
        args->port = SERVER_PORT;
    }
    args->server_ip = strdup(slash->argv[argi]);

    // if param_sniffer_init has already been called you can not change/set hk_node or logfile args
    param_sniffer_init(logfile, hk_node);
    pthread_create(&vm_push_thread, NULL, &vm_push, args);
    vm_running = 1;
    optparse_del(parser);

    return SLASH_SUCCESS;
}
slash_command_sub(vm, start, vm_start_cmd, "", "Start Victoria Metrics push thread");

static int vm_stop_cmd(struct slash * slash) {

    if (!vm_running) return SLASH_SUCCESS;

    vm_running = 0;

    return SLASH_SUCCESS;
}
slash_command_sub(vm, stop, vm_stop_cmd, "", "Stop Victoria Metrics push thread");
