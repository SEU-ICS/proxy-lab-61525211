#include <stdio.h>
#include "csapp.h"
#include <string.h>
#include <pthread.h>
#include <stdlib.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct cache_node {
    char *url;                  
    char *data;                 
    size_t size;                
    struct cache_node *prev;    
    struct cache_node *next;
} cache_node_t;

typedef struct {
    cache_node_t *head;         
    cache_node_t *tail;         
    size_t total_size;          
    pthread_rwlock_t rwlock;    
} cache_t;

static cache_t cache;

void parse_request(char *buf, char *host, char *port, char *path) {
    char method[16], uri[256], version[16];
    sscanf(buf, "%s %s %s", method, uri, version);
    char *p = strstr(uri, "://");
    p = p ? p + 3 : uri;
    char *slash = strchr(p, '/');
    if (slash) {
        strcpy(path, slash);
        *slash = '\0';
    } else {
        strcpy(path, "/");
    }
    char *colon = strchr(p, ':');
    if (colon) {
        *colon = '\0';
        strcpy(port, colon + 1);
    } else {
        strcpy(port, "80");
    }
    strcpy(host, p);
}

void cache_init(cache_t *c) {
    c->head = c->tail = NULL;
    c->total_size = 0;
    pthread_rwlock_init(&c->rwlock, NULL);
}

void build_cache_key(char *key, const char *host, const char *port, const char *path) {
    sprintf(key, "http://%s:%s%s", host, port, path);
}

char *cache_get(cache_t *c, const char *url, size_t *size) {
    pthread_rwlock_rdlock(&c->rwlock);
    cache_node_t *node = c->head;
    while (node) {
        if (strcmp(node->url, url) == 0) {
            pthread_rwlock_unlock(&c->rwlock);
            pthread_rwlock_wrlock(&c->rwlock);
            cache_node_t *cur = c->head;
            while (cur && strcmp(cur->url, url) != 0) cur = cur->next;
            if (!cur) {
                pthread_rwlock_unlock(&c->rwlock);
                return NULL;
            }
            printf("Cache hit: %s\n", url);
            if (cur->prev) cur->prev->next = cur->next;
            if (cur->next) cur->next->prev = cur->prev;
            if (cur == c->tail) c->tail = cur->prev;
            cur->next = c->head;
            cur->prev = NULL;
            if (c->head) c->head->prev = cur;
            c->head = cur;
            if (!c->tail) c->tail = cur;
            char *data = malloc(cur->size);
            memcpy(data, cur->data, cur->size);
            *size = cur->size;
            pthread_rwlock_unlock(&c->rwlock);
            return data;
        }
        node = node->next;
    }
    pthread_rwlock_unlock(&c->rwlock);
    return NULL;
}

void cache_put(cache_t *c, const char *url, const char *data, size_t size) {
    if (size > MAX_OBJECT_SIZE) return;

    pthread_rwlock_wrlock(&c->rwlock);

    cache_node_t *node = c->head;
    while (node) {
        if (strcmp(node->url, url) == 0) {
            free(node->data);
            node->data = malloc(size);
            memcpy(node->data, data, size);
            node->size = size;
            if (node->prev) node->prev->next = node->next;
            if (node->next) node->next->prev = node->prev;
            if (node == c->tail) c->tail = node->prev;
            node->next = c->head;
            node->prev = NULL;
            if (c->head) c->head->prev = node;
            c->head = node;
            if (!c->tail) c->tail = node;
            pthread_rwlock_unlock(&c->rwlock);
            return;
        }
        node = node->next;
    }

    cache_node_t *new_node = malloc(sizeof(cache_node_t));
    new_node->url = malloc(strlen(url) + 1);
    strcpy(new_node->url, url);
    new_node->data = malloc(size);
    memcpy(new_node->data, data, size);
    new_node->size = size;
    new_node->prev = NULL;
    new_node->next = c->head;
    if (c->head) c->head->prev = new_node;
    c->head = new_node;
    if (!c->tail) c->tail = new_node;
    c->total_size += size;

    while (c->total_size > MAX_CACHE_SIZE) {
        cache_node_t *old = c->tail;
        if (!old) break;
        c->tail = old->prev;
        if (c->tail) c->tail->next = NULL;
        else c->head = NULL;
        c->total_size -= old->size;
        free(old->url);
        free(old->data);
        free(old);
    }

    pthread_rwlock_unlock(&c->rwlock);
}

void handle_client(int connfd) {
    rio_t rio;
    char buf[MAXLINE];
    
    Rio_readinitb(&rio, connfd);
    if (Rio_readlineb(&rio, buf, MAXLINE) <= 0) {
        Close(connfd);
        return;
    }
    printf("Received request: %s", buf);
    
    char host[256], port[16], path[256];
    parse_request(buf, host, port, path);
    

    char cache_key[512];
    build_cache_key(cache_key, host, port, path);

    size_t cached_size;
    char *cached_data = cache_get(&cache, cache_key, &cached_size);
    if (cached_data) {
        rio_writen(connfd, cached_data, cached_size);
        free(cached_data);
        Close(connfd);
        return;
    }

    int serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        printf("Cannot connect to %s:%s\n", host, port);
        Close(connfd);
        return;
    }
    
    char request[8192];
    sprintf(request, "GET %s HTTP/1.0\r\n", path);
    sprintf(request + strlen(request), "Host: %s\r\n", host);
    sprintf(request + strlen(request), "%s", user_agent_hdr);
    sprintf(request + strlen(request), "Connection: close\r\n");
    sprintf(request + strlen(request), "Proxy-Connection: close\r\n");
    strcat(request, "\r\n");
    
    rio_writen(serverfd, request, strlen(request));
    
    rio_t server_rio;
    Rio_readinitb(&server_rio, serverfd);
    char resp_buf[MAXLINE];
    ssize_t n;
    size_t total = 0;
    char *response_data = NULL;
    
    while ((n = rio_readlineb(&server_rio, resp_buf, MAXLINE)) > 0) {
        rio_writen(connfd, resp_buf, n);
        if (total + n <= MAX_OBJECT_SIZE) {
            response_data = realloc(response_data, total + n);
            memcpy(response_data + total, resp_buf, n);
            total += n;
        } else {
            if (response_data) {
                free(response_data);
                response_data = NULL;
                total = 0;
            }
        }
    }

    if (response_data && total > 0 && total <= MAX_OBJECT_SIZE) {
        cache_put(&cache, cache_key, response_data, total);
    }
    if (response_data) free(response_data);
    
    Close(serverfd);
    Close(connfd);
}

void *thread_routine(void *arg) {
    int connfd = *(int *)arg;
    free(arg);
    handle_client(connfd);
    return NULL;
}

int main(int argc, char **argv)
{
    Signal(SIGPIPE, SIG_IGN);
    
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port number>\n", argv[0]); 
        return 1;
    }
    
    cache_init(&cache);
    int listenfd = open_listenfd(argv[1]);
    printf("Proxy is running, listening on port %s ...\n", argv[1]); 
    
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int *connfd_ptr = malloc(sizeof(int));
	*connfd_ptr = Accept(listenfd, (SA *)&client_addr, &client_len);
	pthread_t tid;
	Pthread_create(&tid, NULL, thread_routine, connfd_ptr);
	Pthread_detach(tid);
    }
    
    close(listenfd);
    return 0;
}
