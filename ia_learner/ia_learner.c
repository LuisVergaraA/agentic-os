/*
 * ia_learner.c  —  Agentic-OS
 *
 * Servidor TCP multi-hilo. Por cada conexión lanza un hilo dedicado.
 * Protocolo de mensajes (terminados en \n):
 *   "TOTAL <n>"   → el Launcher avisa cuántas ventanas vienen (conexión corta)
 *   "ID <n>"      → una ventana se identifica
 *   "LINE <text>" → oración de texto de una ventana
 *   "EOF"         → la ventana cerró
 *
 * Compilación:
 *   gcc ia_learner.c -o ia_learner -lpthread -Wall -Wextra -I../include
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <ctype.h>

#include "../include/common.h"

/* ═══════════════════════════════════════════════════════════════
 * MUTEX GLOBAL DE CONSOLA
 * Todos los printf del programa usan PRINT_LOCK/PRINT_UNLOCK
 * para evitar salida entrelazada entre hilos.
 * ═══════════════════════════════════════════════════════════════ */
static pthread_mutex_t g_print_mutex = PTHREAD_MUTEX_INITIALIZER;
#define PRINT_LOCK()   pthread_mutex_lock(&g_print_mutex)
#define PRINT_UNLOCK() pthread_mutex_unlock(&g_print_mutex)

/* ═══════════════════════════════════════════════════════════════
 * VARIABLES GLOBALES
 * ═══════════════════════════════════════════════════════════════ */
static volatile sig_atomic_t g_running        = 1;  /* loop principal */
static volatile sig_atomic_t g_total_expected = 0;  /* declarado por TOTAL */

/* ═══════════════════════════════════════════════════════════════
 * DICCIONARIOS BAG-OF-WORDS
 * ═══════════════════════════════════════════════════════════════ */
static const char *DICT_EMAIL[] = {
    "thank","please","regards","meeting","attached",
    "information","update","schedule","team","project", NULL
};
static const char *DICT_ARTICLE[] = {
    "data","analysis","results","method","study",
    "model","research","system","significant","effect", NULL
};
static const char *DICT_REPORT[] = {
    "system","data","network","security","application",
    "server","user","performance","service","infrastructure", NULL
};
static const char **DICTIONARIES[NUM_DOC_CLASSES] = {
    DICT_EMAIL, DICT_ARTICLE, DICT_REPORT
};
static const char *CLASS_NAMES[NUM_DOC_CLASSES] = {
    "Correo electronico", "Articulo cientifico", "Reporte"
};
static const char *USER_TYPE_NAMES[] = {
    "Personal administrativo", "Personal tecnico",
    "Profesor", "Estudiante", "Indeterminado"
};

/* ═══════════════════════════════════════════════════════════════
 * TDA: BAG OF WORDS
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    WordFreq entries[MAX_VOCAB_SIZE];
    int      size;
} BagOfWords;

static void bow_init(BagOfWords *b)
{
    memset(b, 0, sizeof(BagOfWords));
}

static void to_lower(const char *src, char *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

static void bow_add_word(BagOfWords *b, const char *word)
{
    if (!word || !word[0]) return;
    char low[MAX_WORD_LEN];
    to_lower(word, low, sizeof(low));
    for (int i = 0; i < b->size; i++) {
        if (strncmp(b->entries[i].word, low, MAX_WORD_LEN) == 0) {
            b->entries[i].count++;
            return;
        }
    }
    if (b->size < MAX_VOCAB_SIZE) {
        strncpy(b->entries[b->size].word, low, MAX_WORD_LEN - 1);
        b->entries[b->size].count = 1;
        b->size++;
    }
}

static int bow_get_freq(const BagOfWords *b, const char *word)
{
    char low[MAX_WORD_LEN];
    to_lower(word, low, sizeof(low));
    for (int i = 0; i < b->size; i++)
        if (strncmp(b->entries[i].word, low, MAX_WORD_LEN) == 0)
            return b->entries[i].count;
    return 0;
}

static void bow_process_sentence(BagOfWords *b, const char *sentence)
{
    if (!sentence) return;
    char buf[MSG_MAX_LEN];
    strncpy(buf, sentence, MSG_MAX_LEN - 1);
    buf[MSG_MAX_LEN - 1] = '\0';
    char *sp = NULL;
    char *tok = strtok_r(buf, " \t\n\r,.;:!?\"'()[]{}\\/-", &sp);
    while (tok) {
        if (strlen(tok) > 0) bow_add_word(b, tok);
        tok = strtok_r(NULL, " \t\n\r,.;:!?\"'()[]{}\\/-", &sp);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * TDA: DOCUMENTO
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    int             window_id;
    BagOfWords      bow;
    DocClass        doc_class;
    int             in_use;
    pthread_mutex_t mutex;
} DocRecord;

/* ═══════════════════════════════════════════════════════════════
 * TDA: TABLA DE DOCUMENTOS
 * ═══════════════════════════════════════════════════════════════ */
typedef struct {
    DocRecord       docs[MAX_WINDOWS];
    int             count;      /* documentos registrados  */
    int             finished;   /* documentos terminados   */
    pthread_mutex_t mutex;
    pthread_cond_t  all_done;
} DocTable;

static DocTable g_table;

static void doc_table_init(DocTable *t)
{
    memset(t, 0, sizeof(DocTable));
    pthread_mutex_init(&t->mutex, NULL);
    pthread_cond_init(&t->all_done, NULL);
    for (int i = 0; i < MAX_WINDOWS; i++) {
        pthread_mutex_init(&t->docs[i].mutex, NULL);
        t->docs[i].in_use    = 0;
        t->docs[i].doc_class = DOC_UNKNOWN;
        bow_init(&t->docs[i].bow);
    }
}

static DocRecord *doc_table_register(DocTable *t, int window_id)
{
    pthread_mutex_lock(&t->mutex);
    DocRecord *slot = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!t->docs[i].in_use) {
            slot = &t->docs[i];
            slot->in_use    = 1;
            slot->window_id = window_id;
            slot->doc_class = DOC_UNKNOWN;
            bow_init(&slot->bow);
            t->count++;
            /* despertar al clasificador: ya hay al menos 1 doc */
            pthread_cond_signal(&t->all_done);
            break;
        }
    }
    pthread_mutex_unlock(&t->mutex);
    return slot;
}

static void doc_table_mark_done(DocTable *t)
{
    pthread_mutex_lock(&t->mutex);
    t->finished++;
    pthread_cond_broadcast(&t->all_done);
    pthread_mutex_unlock(&t->mutex);
}

/*
 * Esperar a que todos los documentos declarados por TOTAL terminen.
 * Si TOTAL no llegó aún, esperar a que llegue y luego a que terminen.
 */
static void doc_table_wait_all(DocTable *t)
{
    pthread_mutex_lock(&t->mutex);
    /*
     * Esperar la condición correcta según si el Launcher envió TOTAL:
     *   - Con TOTAL: esperar que finished alcance total_expected
     *   - Sin TOTAL: esperar al menos 1 doc y que todos terminen
     * En ambos casos usamos un único cond_wait que se reevalúa
     * cada vez que algo cambia (registro, finish, o llegada de TOTAL).
     */
    for (;;) {
        int target = (int)g_total_expected;
        if (target > 0) {
            /* Launcher declaró cuántas ventanas hay */
            if (t->finished >= target) break;
        } else {
            /* sin TOTAL: esperar al menos 1 y que todas terminen */
            if (t->count > 0 && t->finished >= t->count) break;
        }
        pthread_cond_wait(&t->all_done, &t->mutex);
    }
    pthread_mutex_unlock(&t->mutex);
}

static void doc_table_destroy(DocTable *t)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        pthread_mutex_destroy(&t->docs[i].mutex);
    pthread_mutex_destroy(&t->mutex);
    pthread_cond_destroy(&t->all_done);
}

/* ═══════════════════════════════════════════════════════════════
 * CLASIFICADOR BAG-OF-WORDS
 * ═══════════════════════════════════════════════════════════════ */
static int score_class(const BagOfWords *b, const char **dict, int *matches)
{
    int freq = 0, m = 0;
    for (int i = 0; dict[i]; i++) {
        int f = bow_get_freq(b, dict[i]);
        if (f > 0) { freq += f; m++; }
    }
    if (matches) *matches = m;
    return freq;
}

static DocClass classify_document(const BagOfWords *b, int window_id)
{
    int best = DOC_UNKNOWN, best_freq = -1;

    PRINT_LOCK();
    printf("\n[IALearner] Clasificando documento de ventana %d:\n", window_id);
    for (int c = 0; c < NUM_DOC_CLASSES; c++) {
        int m = 0;
        int f = score_class(b, DICTIONARIES[c], &m);
        printf("  Clase %-22s -> %d palabras, frecuencia = %d",
               CLASS_NAMES[c], m, f);
        if (m >= MIN_DICT_MATCHES) {
            printf(" [ELEGIBLE]");
            if (f > best_freq) { best_freq = f; best = c; }
        }
        printf("\n");
    }
    if (best == DOC_UNKNOWN)
        printf("  -> No clasificable (menos de %d coincidencias)\n",
               MIN_DICT_MATCHES);
    else
        printf("  -> Clase asignada: %s\n", CLASS_NAMES[best]);
    PRINT_UNLOCK();

    return (DocClass)best;
}

/* ═══════════════════════════════════════════════════════════════
 * INFERENCIA DE TIPO DE USUARIO
 * ═══════════════════════════════════════════════════════════════ */
static UserType infer_user_type(const DocTable *t)
{
    int email = 0, article = 0, report = 0, total = 0;

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!t->docs[i].in_use) continue;
        total++;
        pthread_mutex_lock((pthread_mutex_t *)&t->docs[i].mutex);
        DocClass cls = t->docs[i].doc_class;
        pthread_mutex_unlock((pthread_mutex_t *)&t->docs[i].mutex);
        switch (cls) {
            case DOC_EMAIL:   email++;   break;
            case DOC_ARTICLE: article++; break;
            case DOC_REPORT:  report++;  break;
            default: break;
        }
    }

    PRINT_LOCK();
    printf("\n[IALearner] Resumen de documentos:\n");
    printf("  Correos      : %d\n", email);
    printf("  Articulos    : %d\n", article);
    printf("  Reportes     : %d\n", report);
    printf("  Total        : %d\n\n", total);
    PRINT_UNLOCK();

    if (total == 0) return USER_UNKNOWN;

    int has_email   = (email   > 0);
    int has_article = (article > 0);
    int has_report  = (report  > 0);

    if  (has_email && !has_article && !has_report) return USER_ADMIN;
    if  (has_email && !has_article &&  has_report) return USER_TECH;
    if  (has_email &&  has_article && !has_report) return USER_TEACHER;
    if (!has_email &&  has_article &&  has_report) return USER_STUDENT;

    /* Combinación ambigua (ej: los 3 tipos presentes) → indeterminado */
    return USER_UNKNOWN;
}

/* ═══════════════════════════════════════════════════════════════
 * HILO CLASIFICADOR FINAL
 * Espera a que todos los documentos terminen y muestra resultado.
 * ═══════════════════════════════════════════════════════════════ */
static void *classifier_thread(void *arg)
{
    (void)arg;
    PRINT_LOCK();
    printf("[IALearner] Clasificador final esperando documentos...\n");
    PRINT_UNLOCK();

    doc_table_wait_all(&g_table);

    UserType user = infer_user_type(&g_table);

    PRINT_LOCK();
    printf("╔══════════════════════════════════════╗\n");
    printf("║  CONTEXTO DE USUARIO DETECTADO       ║\n");
    printf("║  -> %-34s║\n", USER_TYPE_NAMES[user]);
    printf("╚══════════════════════════════════════╝\n\n");
    PRINT_UNLOCK();

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * HILO POR CONEXIÓN
 * ═══════════════════════════════════════════════════════════════ */
typedef struct { int client_fd; } ThreadArg;

static void *connection_thread(void *arg)
{
    ThreadArg *targ = (ThreadArg *)arg;
    int fd = targ->client_fd;
    free(targ);

    char    buf[MSG_MAX_LEN];
    ssize_t nbytes;
    int     window_id = -1;
    DocRecord *doc    = NULL;

    PRINT_LOCK();
    printf("[IALearner] Nueva conexion fd=%d\n", fd);
    PRINT_UNLOCK();

    while ((nbytes = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[nbytes] = '\0';


        char *sp   = NULL;
        char *line = strtok_r(buf, "\n", &sp);

        while (line != NULL) {
            /* quitar \r residual */
            size_t ll = strlen(line);
            if (ll > 0 && line[ll-1] == '\r') line[--ll] = '\0';
            if (ll == 0) { line = strtok_r(NULL, "\n", &sp); continue; }

            /* ── TOTAL: mensaje del Launcher, no es un documento ── */
            if (strncmp(line, MSG_PREFIX_TOTAL,
                        strlen(MSG_PREFIX_TOTAL)) == 0) {
                char *e;
                long v = strtol(line + strlen(MSG_PREFIX_TOTAL) + 1, &e, 10);
                if (e != line + strlen(MSG_PREFIX_TOTAL) + 1 && v > 0) {
                    g_total_expected = (sig_atomic_t)v;
                    /* despertar al clasificador para que evalúe la condición */
                    pthread_mutex_lock(&g_table.mutex);
                    pthread_cond_broadcast(&g_table.all_done);
                    pthread_mutex_unlock(&g_table.mutex);
                    PRINT_LOCK();
                    printf("[IALearner] Esperando %ld ventana(s)\n", v);
                    PRINT_UNLOCK();
                }
                close(fd);
                return NULL;   /* conexión del Launcher: no registrar doc */
            }

            /* ── ID: una ventana se identifica ── */
            else if (strncmp(line, MSG_PREFIX_ID,
                             strlen(MSG_PREFIX_ID)) == 0) {
                char *e;
                long v = strtol(line + strlen(MSG_PREFIX_ID) + 1, &e, 10);
                if (e == line + strlen(MSG_PREFIX_ID) + 1
                    || v <= 0 || v > MAX_WINDOWS) {
                    fprintf(stderr, "[IALearner] ID invalido\n");
                    close(fd); return NULL;
                }
                window_id = (int)v;
                doc = doc_table_register(&g_table, window_id);
                if (!doc) {
                    fprintf(stderr, "[IALearner] Tabla llena\n");
                    close(fd); return NULL;
                }
                PRINT_LOCK();
                printf("[IALearner] Ventana %d registrada\n", window_id);
                PRINT_UNLOCK();
            }

            /* ── LINE: oración de texto ── */
            else if (strncmp(line, MSG_PREFIX_LINE,
                             strlen(MSG_PREFIX_LINE)) == 0) {
                const char *text = line + strlen(MSG_PREFIX_LINE) + 1;
                if (doc) {
                    pthread_mutex_lock(&doc->mutex);
                    bow_process_sentence(&doc->bow, text);
                    pthread_mutex_unlock(&doc->mutex);
                    PRINT_LOCK();
                    printf("[IALearner] Ventana %d: %s\n", window_id, text);
                    PRINT_UNLOCK();
                }
            }

            /* ── EOF: la ventana cerró ── */
            else if (strncmp(line, MSG_PREFIX_EOF,
                             strlen(MSG_PREFIX_EOF)) == 0) {
                PRINT_LOCK();
                printf("[IALearner] Ventana %d cerrada (EOF)\n", window_id);
                PRINT_UNLOCK();
                goto done;
            }

            line = strtok_r(NULL, "\n", &sp);
        }
    }

done:
    PRINT_LOCK();
    if (window_id >= 0)
        printf("[IALearner] Ventana %d: conexion cerrada\n", window_id);
    PRINT_UNLOCK();
    close(fd);

    if (doc) {
        pthread_mutex_lock(&doc->mutex);
        doc->doc_class = classify_document(&doc->bow, window_id);
        pthread_mutex_unlock(&doc->mutex);
        doc_table_mark_done(&g_table);
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * SEÑAL SIGINT
 * ═══════════════════════════════════════════════════════════════ */
static void handle_sigint(int sig) { (void)sig; g_running = 0; }

/* ═══════════════════════════════════════════════════════════════
 * main
 * ═══════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Uso: %s <puerto>\n  Ejemplo: %s 9000\n",
                argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr;
    long port = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || port <= 0 || port > 65535) {
        fprintf(stderr, "Puerto invalido: %s\n", argv[1]);
        return EXIT_FAILURE;
    }

    signal(SIGINT,  handle_sigint);
    signal(SIGPIPE, SIG_IGN);

    doc_table_init(&g_table);

    /* Hilo clasificador final — joined en main para garantizar output */
    pthread_t clf_tid;
    pthread_create(&clf_tid, NULL, classifier_thread, NULL);

    /* Socket servidor */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return EXIT_FAILURE; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Timeout de 1 s en accept() para poder revisar g_running con Ctrl+C */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(server_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); return EXIT_FAILURE;
    }
    if (listen(server_fd, MAX_WINDOWS) < 0) {
        perror("listen"); close(server_fd); return EXIT_FAILURE;
    }

    printf("[IALearner] Escuchando en puerto %ld... (Ctrl+C para detener)\n\n",
           port);

    while (g_running) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);

        int cfd = accept(server_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == EINTR) break;
            perror("accept"); break;
        }

        /* Quitar timeout heredado del server_fd — el recv() del cliente
         * debe bloquear indefinidamente esperando mensajes del x11_client */
        struct timeval notv = { .tv_sec = 0, .tv_usec = 0 };
        setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &notv, sizeof(notv));

        ThreadArg *targ = malloc(sizeof(ThreadArg));
        if (!targ) { close(cfd); continue; }
        targ->client_fd = cfd;

        pthread_t tid;
        if (pthread_create(&tid, NULL, connection_thread, targ) != 0) {
            perror("pthread_create"); free(targ); close(cfd); continue;
        }
        pthread_detach(tid);
    }

    close(server_fd);
    pthread_join(clf_tid, NULL);   /* esperar resultado final antes de salir */
    doc_table_destroy(&g_table);

    printf("[IALearner] Servidor detenido.\n");
    return EXIT_SUCCESS;
}