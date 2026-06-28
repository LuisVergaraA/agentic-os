/*
 * x11_client.c  —  Agentic-OS
 *
 * Ventana X11 que captura teclas y envía oraciones al IALearner via TCP.
 * El envío se hace en un hilo dedicado (sender_thread) desacoplado del
 * event-loop de X11 mediante una cola productor-consumidor.
 *
 * Compilación:
 *   gcc x11_client.c -o x11_client -lX11 -lpthread -Wall -Wextra -I../include
 *
 * Uso:
 *   ./x11_client <host> <puerto> <window_id>
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>

#include "../include/common.h"

/* ── Cola productor-consumidor ────────────────────────────────── */
#define QUEUE_CAP 64

typedef struct {
    char   items[QUEUE_CAP][MSG_MAX_LEN];
    int    head, tail, count;
    int    done;                  /* 1 = no llegan más mensajes     */
    pthread_mutex_t mtx;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} Queue;

static void queue_init(Queue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mtx,       NULL);
    pthread_cond_init(&q->not_empty,  NULL);
    pthread_cond_init(&q->not_full,   NULL);
}

static void queue_push(Queue *q, const char *msg) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == QUEUE_CAP)
        pthread_cond_wait(&q->not_full, &q->mtx);
    strncpy(q->items[q->tail], msg, MSG_MAX_LEN - 1);
    q->items[q->tail][MSG_MAX_LEN - 1] = '\0';
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

/* pop bloqueante; retorna 0 si no hay más mensajes */
static int queue_pop(Queue *q, char *out) {
    pthread_mutex_lock(&q->mtx);
    /* Esperar mientras la cola esté vacía Y no haya terminado.
     * Si done=1 pero quedan items, seguir procesándolos primero. */
    while (q->count == 0 && !q->done)
        pthread_cond_wait(&q->not_empty, &q->mtx);
    if (q->count == 0) {   /* solo salir cuando vacía Y done */
        pthread_mutex_unlock(&q->mtx);
        return 0;
    }
    strncpy(out, q->items[q->head], MSG_MAX_LEN - 1);
    out[MSG_MAX_LEN - 1] = '\0';
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mtx);
    return 1;
}

static void queue_close(Queue *q) {
    pthread_mutex_lock(&q->mtx);
    q->done = 1;
    pthread_cond_broadcast(&q->not_empty);
    pthread_mutex_unlock(&q->mtx);
}

static void queue_destroy(Queue *q) {
    pthread_mutex_destroy(&q->mtx);
    pthread_cond_destroy(&q->not_empty);
    pthread_cond_destroy(&q->not_full);
}

/* ── Estado global del cliente ────────────────────────────────── */
typedef struct {
    int    sock_fd;
    int    window_id;
    Queue  queue;
} ClientCtx;

static ClientCtx g_ctx;

/* ── Hilo enviador ─────────────────────────────────────────────── */
static void *sender_thread(void *arg)
{
    (void)arg;
    char msg[MSG_MAX_LEN];

    while (queue_pop(&g_ctx.queue, msg)) {
        if (g_ctx.sock_fd < 0) continue;   /* sin conexión, descartar */

        /* asegurar \n al final */
        size_t len = strlen(msg);
        if (len == 0) continue;
        if (msg[len-1] != '\n' && len < MSG_MAX_LEN - 1) {
            msg[len]   = '\n';
            msg[len+1] = '\0';
            len++;
        }

        ssize_t sent = send(g_ctx.sock_fd, msg, len, MSG_NOSIGNAL);
        if (sent < 0)
            fprintf(stderr, "[x11_client-%d] send error: %s\n",
                    g_ctx.window_id, strerror(errno));
    }
    return NULL;
}

/* ── Conectar al IALearner ─────────────────────────────────────── */
static int tcp_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[x11_client] Dirección inválida: %s\n", host);
        close(fd); return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[x11_client-%d] No se pudo conectar a %s:%d — %s\n",
                0, host, port, strerror(errno));
        close(fd); return -1;
    }
    return fd;
}

/* ── Dibujar pista visual ──────────────────────────────────────── */
static void draw_hint(Display *dpy, Window win, GC gc, int wid)
{
    char hint1[64], hint2[64];
    snprintf(hint1, sizeof(hint1), "Ventana %d — Escribe y pulsa Enter", wid);
    snprintf(hint2, sizeof(hint2), "Escape cierra la ventana");
    XDrawString(dpy, win, gc, 20, 50, hint1, (int)strlen(hint1));
    XDrawString(dpy, win, gc, 20, 75, hint2, (int)strlen(hint2));
}

/* ── main ──────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    /* validar argumentos */
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <host> <puerto> <window_id>\n", argv[0]);
        return EXIT_FAILURE;
    }
    const char *host = argv[1];

    char *ep;
    long port = strtol(argv[2], &ep, 10);
    if (*ep != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto inválido: %s\n", argv[2]);
        return EXIT_FAILURE;
    }
    long wid = strtol(argv[3], &ep, 10);
    if (*ep != '\0' || wid <= 0 || wid > MAX_WINDOWS) {
        fprintf(stderr, "window_id inválido: %s\n", argv[3]);
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    /* inicializar contexto */
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.window_id = (int)wid;
    g_ctx.sock_fd   = -1;
    queue_init(&g_ctx.queue);

    /* conectar al servidor */
    g_ctx.sock_fd = tcp_connect(host, (int)port);
    if (g_ctx.sock_fd < 0)
        fprintf(stderr, "[x11_client-%ld] Sin conexión — modo local\n", wid);

    /* lanzar hilo enviador */
    pthread_t tid;
    if (pthread_create(&tid, NULL, sender_thread, NULL) != 0) {
        perror("pthread_create");
        if (g_ctx.sock_fd >= 0) close(g_ctx.sock_fd);
        queue_destroy(&g_ctx.queue);
        return EXIT_FAILURE;
    }

    /* primer mensaje: identificarse ante el IALearner */
    if (g_ctx.sock_fd >= 0) {
        char id_msg[32];
        snprintf(id_msg, sizeof(id_msg), "ID %ld", wid);
        queue_push(&g_ctx.queue, id_msg);
    }

    /* abrir display X11 */
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "[x11_client-%ld] No se puede abrir display X11\n", wid);
        queue_close(&g_ctx.queue);
        pthread_join(tid, NULL);
        if (g_ctx.sock_fd >= 0) close(g_ctx.sock_fd);
        queue_destroy(&g_ctx.queue);
        return EXIT_FAILURE;
    }

    int scr = DefaultScreen(dpy);
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                                     10 + (int)wid*20, 10 + (int)wid*20,
                                     500, 200, 2,
                                     BlackPixel(dpy, scr),
                                     WhitePixel(dpy, scr));
    char title[64];
    snprintf(title, sizeof(title), "Agentic-OS — Ventana %ld", wid);
    XStoreName(dpy, win, title);
    XSelectInput(dpy, win, ExposureMask | KeyPressMask);
    XMapWindow(dpy, win);
    GC gc = XCreateGC(dpy, win, 0, NULL);

    /* buffer de oración */
    char sentence[MAX_SENTENCE_LEN];
    int  slen = 0;
    memset(sentence, 0, sizeof(sentence));

    /* event loop */
    XEvent ev;
    int running = 1;

    while (running) {
        XNextEvent(dpy, &ev);

        if (ev.type == Expose) {
            XClearWindow(dpy, win);
            draw_hint(dpy, win, gc, (int)wid);
            if (slen > 0)
                XDrawString(dpy, win, gc, 20, 130, sentence, slen);
            continue;
        }

        if (ev.type != KeyPress) continue;

        char   buf[32] = {0};
        KeySym ks;
        int    nc = XLookupString(&ev.xkey, buf, sizeof(buf)-1, &ks, NULL);

        /* Escape: cerrar */
        if (ks == XK_Escape) { running = 0; break; }

        /* Enter: marcar fin de oración */
        if (ks == XK_Return || ks == XK_KP_Enter) {
            /* si quedó una palabra sin espacio al final, enviarla primero */
            if (slen > 0 && g_ctx.sock_fd >= 0) {
                char msg[MSG_MAX_LEN];
                snprintf(msg, sizeof(msg), "%s ", MSG_PREFIX_WORD);
                strncat(msg, sentence, sizeof(msg) - strlen(msg) - 1);
                queue_push(&g_ctx.queue, msg);
                printf("[x11_client-%d] Palabra final: %s\n",
                       g_ctx.window_id, sentence);
            }
            /* señal de fin de oración al IALearner */
            if (g_ctx.sock_fd >= 0)
                queue_push(&g_ctx.queue, MSG_PREFIX_NEWLINE);
            printf("[x11_client-%d] --- fin de oración ---\n", g_ctx.window_id);
            memset(sentence, 0, sizeof(sentence));
            slen = 0;
            XClearWindow(dpy, win);
            draw_hint(dpy, win, gc, (int)wid);
            continue;
        }

        /* Backspace */
        if (ks == XK_BackSpace) {
            if (slen > 0) sentence[--slen] = '\0';
            XClearWindow(dpy, win);
            draw_hint(dpy, win, gc, (int)wid);
            if (slen > 0) XDrawString(dpy, win, gc, 20, 130, sentence, slen);
            continue;
        }

        /* carácter imprimible */
        if (nc > 0 && (unsigned char)buf[0] >= 32 && (unsigned char)buf[0] < 127) {
            /* Espacio: enviar la palabra acumulada y resetear buffer */
            if (buf[0] == ' ') {
                if (slen > 0 && g_ctx.sock_fd >= 0) {
                    char msg[MSG_MAX_LEN];
                    snprintf(msg, sizeof(msg), "%s ", MSG_PREFIX_WORD);
                    strncat(msg, sentence, sizeof(msg) - strlen(msg) - 1);
                    queue_push(&g_ctx.queue, msg);
                    printf("[x11_client-%d] Palabra: %s\n",
                           g_ctx.window_id, sentence);
                }
                memset(sentence, 0, sizeof(sentence));
                slen = 0;
                XClearWindow(dpy, win);
                draw_hint(dpy, win, gc, (int)wid);
            } else if (slen + nc < (int)sizeof(sentence) - 1) {
                /* acumular letra en el buffer */
                memcpy(sentence + slen, buf, nc);
                slen += nc;
                sentence[slen] = '\0';
                XClearWindow(dpy, win);
                draw_hint(dpy, win, gc, (int)wid);
                XDrawString(dpy, win, gc, 20, 130, sentence, slen);
            }
        }
    }

    /* enviar palabra parcial si quedó texto al cerrar */
    if (slen > 0 && g_ctx.sock_fd >= 0) {
        char msg[MSG_MAX_LEN];
        snprintf(msg, sizeof(msg), "%s ", MSG_PREFIX_WORD);
        strncat(msg, sentence, sizeof(msg) - strlen(msg) - 1);
        queue_push(&g_ctx.queue, msg);
        /* marcar fin de oración implícito al cerrar */
        queue_push(&g_ctx.queue, MSG_PREFIX_NEWLINE);
    }

    /* ORDEN CRÍTICO:
     * 1. queue_close + pthread_join PRIMERO — garantiza que el hilo
     *    envía todos los mensajes pendientes antes de cerrar el socket.
     * 2. shutdown + close del socket — el servidor detecta recv()==0.
     * 3. XCloseDisplay AL FINAL — evita que X11 bloquee el cierre TCP. */

    queue_close(&g_ctx.queue);
    pthread_join(tid, NULL);

    if (g_ctx.sock_fd >= 0) {
        shutdown(g_ctx.sock_fd, SHUT_WR);
        close(g_ctx.sock_fd);
        g_ctx.sock_fd = -1;
    }

    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    queue_destroy(&g_ctx.queue);

    printf("[x11_client-%d] Recursos liberados. Saliendo.\n", g_ctx.window_id);
    return EXIT_SUCCESS;
}