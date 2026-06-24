/*
 * launcher.c
 * Proceso Launcher — Agentic-OS
 *
 * Consola interactiva que crea N clientes X11, monitorea su estado
 * y coordina el cierre limpio de todo el sistema.
 *
 * Antes de lanzar las ventanas, notifica al IALearner cuántas
 * conexiones esperar ("TOTAL <n>\n") para que el clasificador
 * final sepa cuándo mostrar el resultado.
 *
 * Compilación:
 *   gcc launcher.c -o launcher -Wall -Wextra -I../include
 *
 * Uso:
 *   ./launcher <n_ventanas> [host_ialearner] [puerto]
 *   Ejemplo: ./launcher 3 127.0.0.1 9000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <errno.h>
#include <sys/prctl.h>

#include "../include/common.h"

/* ════════════════════════════════════════════════════════════════
 * TABLA DE PROCESOS HIJOS
 * ════════════════════════════════════════════════════════════════ */
static ManagedProcess g_procs[MAX_WINDOWS];
static volatile sig_atomic_t g_proc_count = 0;
static volatile sig_atomic_t g_finished   = 0;

/* ════════════════════════════════════════════════════════════════
 * HANDLER DE SIGCHLD
 * Loop obligatorio: si dos hijos mueren casi a la vez, las señales
 * SIGCHLD pueden colapsar en una sola. Sin el loop se perdería un
 * waitpid() y quedaría un zombie.
 * ════════════════════════════════════════════════════════════════ */
static void sigchld_handler(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        for (int i = 0; i < g_proc_count; i++) {
            if (g_procs[i].pid == pid) {
                g_procs[i].state     = PROC_FINISHED;
                g_procs[i].exit_code = WIFEXITED(status)
                                       ? WEXITSTATUS(status) : -1;
                g_finished++;
                break;
            }
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 * NOTIFICAR TOTAL AL IALEARNER
 * Conexión TCP corta: solo envía "TOTAL <n>\n" y cierra.
 * ════════════════════════════════════════════════════════════════ */
static int notify_total(const char *host, int port, int n)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket (TOTAL)"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "[Launcher] Host inválido: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[Launcher] No se pudo conectar a IALearner "
                "(%s:%d): %s\n", host, port, strerror(errno));
        close(fd);
        return -1;
    }

    char msg[MSG_MAX_LEN];
    snprintf(msg, sizeof(msg), "%s %d\n", MSG_PREFIX_TOTAL, n);
    send(fd, msg, strlen(msg), MSG_NOSIGNAL);
    close(fd);

    printf("[Launcher] Notificado a IALearner: esperando %d ventana(s)\n", n);
    return 0;
}

/* ════════════════════════════════════════════════════════════════
 * LANZAR UN CLIENTE X11
 * fork() + execv(). prctl() garantiza SIGTERM al hijo si el padre
 * muere — previene procesos huérfanos.
 * ════════════════════════════════════════════════════════════════ */
static pid_t launch_client(int window_id,
                            const char *host,
                            int port,
                            const char *client_bin)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* ── proceso hijo ── */
        prctl(PR_SET_PDEATHSIG, SIGTERM);

        char id_str[16], port_str[16];
        snprintf(id_str,   sizeof(id_str),   "%d", window_id);
        snprintf(port_str, sizeof(port_str), "%d", port);

        char *args[] = {
            (char *)client_bin,
            (char *)host,
            port_str,
            id_str,
            NULL
        };

        execv(client_bin, args);
        fprintf(stderr, "[Launcher] execv(%s) falló: %s\n",
                client_bin, strerror(errno));
        _exit(EXIT_FAILURE);
    }

    return pid;   /* padre: retorna PID del hijo */
}

/* ════════════════════════════════════════════════════════════════
 * IMPRIMIR TABLA DE ESTADO
 * ════════════════════════════════════════════════════════════════ */
static void print_status(void)
{
    printf("\n┌─────────────────────────────────────────┐\n");
    printf("│         ESTADO DE PROCESOS              │\n");
    printf("├──────┬──────────┬───────────┬───────────┤\n");
    printf("│  ID  │   PID    │  Estado   │ Cod.Sal.  │\n");
    printf("├──────┼──────────┼───────────┼───────────┤\n");

    for (int i = 0; i < g_proc_count; i++) {
        const char *estado = (g_procs[i].state == PROC_RUNNING)
                             ? "CORRIENDO " : "TERMINADO ";
        int cod = (g_procs[i].state == PROC_FINISHED)
                  ? g_procs[i].exit_code : -1;
        printf("│  %2d  │ %8d │ %s│ %9d │\n",
               g_procs[i].window_id, g_procs[i].pid, estado, cod);
    }

    printf("└──────┴──────────┴───────────┴───────────┘\n");
    printf("  Activos: %d  |  Terminados: %d  |  Total: %d\n\n",
           (int)(g_proc_count - g_finished),
           (int)g_finished,
           (int)g_proc_count);
}

/* ════════════════════════════════════════════════════════════════
 * CIERRE LIMPIO DE TODOS LOS HIJOS
 * SIGTERM primero; SIGKILL tras 2 s si siguen vivos.
 * ════════════════════════════════════════════════════════════════ */
static void shutdown_all(void)
{
    printf("[Launcher] Enviando SIGTERM a todos los procesos...\n");

    for (int i = 0; i < g_proc_count; i++)
        if (g_procs[i].state == PROC_RUNNING)
            kill(g_procs[i].pid, SIGTERM);

    /* Esperar hasta 2 s en pasos de 100 ms */
    for (int t = 0; t < 20; t++) {
        usleep(100000);
        int todos_ok = 1;
        for (int i = 0; i < g_proc_count; i++)
            if (g_procs[i].state == PROC_RUNNING) { todos_ok = 0; break; }
        if (todos_ok) break;
    }

    /* SIGKILL a los que siguen vivos */
    int forzados = 0;
    for (int i = 0; i < g_proc_count; i++) {
        if (g_procs[i].state == PROC_RUNNING) {
            kill(g_procs[i].pid, SIGKILL);
            forzados++;
        }
    }
    if (forzados > 0)
        printf("[Launcher] SIGKILL a %d proceso(s) que no respondieron\n",
               forzados);

    /* Recoger zombies residuales */
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0)
        ;

    printf("[Launcher] Todos los procesos terminados.\n");
}

/* ════════════════════════════════════════════════════════════════
 * MENÚ INTERACTIVO
 * ════════════════════════════════════════════════════════════════ */
static void print_menu(void)
{
    printf("╔══════════════════════════════╗\n");
    printf("║      AGENTIC-OS LAUNCHER     ║\n");
    printf("╠══════════════════════════════╣\n");
    printf("║  1. Ver estado de procesos   ║\n");
    printf("║  2. Salir (cierre limpio)    ║\n");
    printf("╚══════════════════════════════╝\n");
    printf("Opción: ");
    fflush(stdout);
}

/* ════════════════════════════════════════════════════════════════
 * main
 * ════════════════════════════════════════════════════════════════ */
int main(int argc, char *argv[])
{
    /* ── Validación de argumentos ── */
    if (argc < 2 || argc > 4) {
        fprintf(stderr,
            "Uso: %s <n_ventanas> [host_ialearner] [puerto]\n"
            "  Ejemplo: %s 3 127.0.0.1 9000\n",
            argv[0], argv[0]);
        return EXIT_FAILURE;
    }

    char *endptr;
    long n_windows = strtol(argv[1], &endptr, 10);
    if (*endptr != '\0' || n_windows < 1 || n_windows > MAX_WINDOWS) {
        fprintf(stderr,
            "n_ventanas debe ser un entero entre 1 y %d\n", MAX_WINDOWS);
        return EXIT_FAILURE;
    }

    const char *host = (argc >= 3) ? argv[2] : IALEARNER_HOST;
    int port = IALEARNER_PORT;
    if (argc == 4) {
        long p = strtol(argv[3], &endptr, 10);
        if (*endptr != '\0' || p <= 0 || p > 65535) {
            fprintf(stderr, "Puerto inválido: %s\n", argv[3]);
            return EXIT_FAILURE;
        }
        port = (int)p;
    }

    /* Ruta al binario del cliente X11 */
    const char *client_bin = "../x11_client/x11_client";
    if (access(client_bin, X_OK) != 0) {
        fprintf(stderr,
            "[Launcher] No se encontró '%s'.\n"
            "  Compila primero: cd ../x11_client && "
            "gcc x11_client.c -o x11_client -lX11 -lpthread\n",
            client_bin);
        return EXIT_FAILURE;
    }

    /* ── Instalar SIGCHLD ── */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    signal(SIGPIPE, SIG_IGN);

    printf("[Launcher] Agentic-OS — %ld ventana(s) | IALearner %s:%d\n\n",
           n_windows, host, port);

    /* ── Notificar TOTAL al IALearner ── */
    if (notify_total(host, port, (int)n_windows) < 0)
        fprintf(stderr, "[Launcher] ADVERTENCIA: sin conexión a IALearner\n");

    /* ── Lanzar los N clientes X11 ── */
    memset(g_procs, 0, sizeof(g_procs));

    for (int i = 0; i < (int)n_windows; i++) {
        int wid = i + 1;
        pid_t pid = launch_client(wid, host, port, client_bin);
        if (pid < 0) {
            fprintf(stderr, "[Launcher] Fallo al lanzar ventana %d\n", wid);
            shutdown_all();
            return EXIT_FAILURE;
        }
        g_procs[i].pid       = pid;
        g_procs[i].window_id = wid;
        g_procs[i].state     = PROC_RUNNING;
        g_procs[i].exit_code = 0;
        g_proc_count++;

        printf("[Launcher] Ventana %d lanzada (PID %d)\n", wid, pid);
        usleep(100000);   /* 100 ms entre lanzamientos */
    }

    printf("\n");

    /* ── Menú interactivo ── */
    char input[64];
    int  running = 1;

    while (running) {
        print_menu();

        if (fgets(input, sizeof(input), stdin) == NULL)
            break;

        input[strcspn(input, "\n")] = '\0';

        char *opt_end;
        long opt = strtol(input, &opt_end, 10);
        if (opt_end == input || *opt_end != '\0') {
            printf("Opción inválida. Ingresa 1 o 2.\n\n");
            continue;
        }

        switch (opt) {
        case 1:
            print_status();
            break;
        case 2:
            running = 0;
            break;
        default:
            printf("Opción inválida. Ingresa 1 o 2.\n\n");
            break;
        }

        /* Salida automática si todos los hijos terminaron */
        if (g_proc_count > 0 && g_finished >= g_proc_count) {
            printf("[Launcher] Todas las ventanas se cerraron.\n");
            running = 0;
        }
    }

    shutdown_all();
    printf("[Launcher] Fin.\n");
    return EXIT_SUCCESS;
}