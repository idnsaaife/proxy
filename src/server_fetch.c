#include "server_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

void *fetch_from_server(void *arg) {
    fetch_info_t *fetch_info = (fetch_info_t *)arg;
    cache_entry_t *entry = fetch_info->entry;
    
    printf("[FETCH] Starting: %s from %s:%d%s\n", 
           fetch_info->url, fetch_info->host, fetch_info->port, fetch_info->path);
    
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Socket creation failed");
        goto error_exit;
    }
    
    struct hostent *server = gethostbyname(fetch_info->host);
    if (server == NULL) {
        printf("Host not found: %s\n", fetch_info->host);
        close(server_socket);
        goto error_exit;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(fetch_info->port);
    
    if (connect(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Connection failed");
        close(server_socket);
        goto error_exit;
    }
    
    char request[2048];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s\r\n"
             "Connection: close\r\n"
             "User-Agent: ProxyCache/1.0\r\n"
             "Accept: */*\r\n"
             "\r\n", fetch_info->path, fetch_info->host);
    
    if (send(server_socket, request, strlen(request), 0) < 0) {
        perror("Send failed");
        close(server_socket);
        goto error_exit;
    }
    
    char buffer[BUFFER_SIZE];
    size_t total_received = 0;
    ssize_t bytes_received;
    
    pthread_mutex_lock(&entry->lock);
    entry->data = malloc(BUFFER_SIZE);
    entry->capacity = BUFFER_SIZE;
    pthread_mutex_unlock(&entry->lock);
    
    while ((bytes_received = recv(server_socket, buffer, BUFFER_SIZE, 0)) > 0) {
        pthread_mutex_lock(&entry->lock);
        
        if (total_received + bytes_received > entry->capacity) {
            size_t new_capacity = entry->capacity * 2;
            if (new_capacity > MAX_OBJECT_SIZE) {
                pthread_mutex_unlock(&entry->lock);
                printf("[FETCH] Object too large\n");
                break;
            }
            char *new_data = realloc(entry->data, new_capacity);
            if (new_data == NULL) {
                pthread_mutex_unlock(&entry->lock);
                break;
            }
            entry->data = new_data;
            entry->capacity = new_capacity;
        }
        
        memcpy(entry->data + total_received, buffer, bytes_received);
        total_received += bytes_received;
        entry->data_size = total_received;
        
        pthread_mutex_unlock(&entry->lock);
    }
    
    close(server_socket);
    printf("[FETCH] Received %zu bytes\n", total_received);
    
    pthread_mutex_lock(&entry->lock);
    entry->in_progress = 0;
    entry->ready = 1;
    if (total_received == 0) {
        entry->error = 1;
    }
    pthread_cond_broadcast(&entry->ready_cond);
    pthread_mutex_unlock(&entry->lock);
    
    if (!entry->error && total_received > 0) {
        finalize_cache_entry(entry);
    }
    
    cache_entry_release(entry);
    free(fetch_info);
    return NULL;

error_exit:
    pthread_mutex_lock(&entry->lock);
    entry->in_progress = 0;
    entry->ready = 1;
    entry->error = 1;
    pthread_cond_broadcast(&entry->ready_cond);
    pthread_mutex_unlock(&entry->lock);
    cache_entry_release(entry);
    free(fetch_info);
    return NULL;
}
