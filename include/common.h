/*
 * common.h
 * Header compartido — Agentic-OS
 *
 * Define las constantes, tipos y estructuras que usan los tres módulos:
 *   launcher, x11_client e ia_learner.
 *
 * NO incluir lógica aquí: solo definiciones y macros.
 */

#ifndef COMMON_H
#define COMMON_H

#include <sys/types.h>   /* pid_t  */
#include <stdint.h>      /* uint16_t */

/* ════════════════════════════════════════════════════════════════
 * RED / PROTOCOLO TCP
 * ════════════════════════════════════════════════════════════════ */

/* Puerto donde escucha IALearner */
#define IALEARNER_PORT        9000

/* IP del servidor IALearner (cambiar en producción) */
#define IALEARNER_HOST        "127.0.0.1"

/* Tamaño máximo de un mensaje en el protocolo TCP.
 * Formato de cada mensaje: "<tipo> <datos>\n"
 * Ejemplos:
 *   "ID 3\n"                   → identificación de ventana
 *   "LINE hola mundo\n"        → línea de texto
 *   "EOF\n"                    → el cliente cerró su ventana
 */
#define MSG_MAX_LEN           1024

/* Prefijos del protocolo (lo que va antes del contenido en cada mensaje) */
#define MSG_PREFIX_ID         "ID"     /* primer mensaje al conectar    */
#define MSG_PREFIX_LINE       "LINE"   /* una oración completa          */
#define MSG_PREFIX_EOF        "EOF"    /* la ventana se cerró           */
#define MSG_PREFIX_WORD       "WORD"   /* una palabra individual        */
#define MSG_PREFIX_NEWLINE    "NL"     /* fin de oración (Enter)        */
#define MSG_PREFIX_TOTAL      "TOTAL"  /* launcher avisa cuántas ventanas habrá */

/* ════════════════════════════════════════════════════════════════
 * LÍMITES GENERALES
 * ════════════════════════════════════════════════════════════════ */

/* Máximo de ventanas/procesos gráficos que puede lanzar el Launcher */
#define MAX_WINDOWS           16

/* Largo máximo de una oración acumulada en el cliente X11 */
#define MAX_SENTENCE_LEN      1024

/* Largo máximo de una palabra individual */
#define MAX_WORD_LEN          64

/* Máximo de palabras distintas que puede tener el vocabulario bag-of-words */
#define MAX_VOCAB_SIZE        512

/* Máximo de oraciones que IALearner almacena por documento */
#define MAX_LINES_PER_DOC     256

/* Cantidad de clases de documento (email, artículo, reporte) */
#define NUM_DOC_CLASSES       3

/* Mínimo de palabras del diccionario que deben aparecer para
 * asignar una clase a un documento (regla del enunciado) */
#define MIN_DICT_MATCHES      3

/* ════════════════════════════════════════════════════════════════
 * TIPOS: ESTADO DE PROCESO (usado por el Launcher)
 * ════════════════════════════════════════════════════════════════ */

typedef enum {
    PROC_RUNNING  = 0,   /* el proceso gráfico sigue vivo   */
    PROC_FINISHED = 1,   /* terminó normalmente             */
    PROC_ERROR    = 2    /* terminó con código de error     */
} ProcessState;

/*
 * Registro de un proceso gráfico en la tabla del Launcher.
 * El Launcher mantiene un arreglo de estos structs.
 */
typedef struct {
    pid_t        pid;          /* PID del proceso hijo              */
    int          window_id;    /* número de ventana (1..N)          */
    ProcessState state;        /* estado actual                     */
    int          exit_code;    /* código de salida (cuando termina) */
} ManagedProcess;

/* ════════════════════════════════════════════════════════════════
 * TIPOS: CLASIFICACIÓN DE DOCUMENTOS (usado por IALearner)
 * ════════════════════════════════════════════════════════════════ */

/*
 * Índices de cada clase de documento.
 * Deben coincidir con el orden en que se declaran los diccionarios
 * en ia_learner.c
 */
typedef enum {
    DOC_EMAIL      = 0,
    DOC_ARTICLE    = 1,
    DOC_REPORT     = 2,
    DOC_UNKNOWN    = -1   /* no se pudo clasificar */
} DocClass;

/*
 * Tipo de usuario inferido a partir del conjunto de documentos.
 * Basado en la tabla del enunciado.
 */
typedef enum {
    USER_ADMIN    = 0,   /* Personal administrativo: solo emails        */
    USER_TECH     = 1,   /* Personal técnico:        emails + reportes  */
    USER_TEACHER  = 2,   /* Profesor:                emails + artículos */
    USER_STUDENT  = 3,   /* Estudiante:              artículos + reportes */
    USER_UNKNOWN  = 4    /* No se pudo determinar                       */
} UserType;

/* ════════════════════════════════════════════════════════════════
 * TIPOS: BAG OF WORDS (usado por IALearner)
 * ════════════════════════════════════════════════════════════════ */

/*
 * Vector de frecuencias de un documento.
 * Cada posición corresponde a una palabra del diccionario de su clase.
 * 'word'  → la palabra del diccionario
 * 'count' → cuántas veces apareció en el documento
 */
typedef struct {
    char word[MAX_WORD_LEN];
    int  count;
} WordFreq;

/*
 * Documento completo procesado por IALearner.
 * Un documento = una ventana X11 = una conexión TCP.
 */
typedef struct {
    int       window_id;                    /* qué ventana generó este doc  */
    WordFreq  bow[MAX_VOCAB_SIZE];          /* bag of words del documento   */
    int       vocab_size;                   /* palabras distintas vistas    */
    DocClass  doc_class;                    /* clase asignada al final      */
    int       active;                       /* 1 = conexión abierta         */
} Document;

/* ════════════════════════════════════════════════════════════════
 * MACROS DE UTILIDAD
 * ════════════════════════════════════════════════════════════════ */

/* Imprimir error con nombre de archivo y línea — útil para depurar */
#define LOG_ERR(msg) \
    fprintf(stderr, "[ERROR %s:%d] %s\n", __FILE__, __LINE__, msg)

/* Verificar condición y retornar si falla (programación defensiva) */
#define CHECK_NULL(ptr, retval) \
    do { if ((ptr) == NULL) { LOG_ERR(#ptr " es NULL"); return (retval); } } while(0)

#endif /* COMMON_H */