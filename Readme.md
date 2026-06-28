# Agentic-OS

Sistema de aprendizaje de contexto de usuario basado en agentes de IA.
Captura texto de ventanas gráficas X11 palabra por palabra, lo clasifica
con bag-of-words en español e infiere el tipo de usuario
(administrativo, técnico, profesor o estudiante).

---

## Estructura del proyecto

```
agentic_os/
├── include/
│   └── common.h          # Constantes, tipos y protocolo compartidos
├── launcher/
│   └── launcher.c        # Proceso padre: menú interactivo + fork de ventanas
├── x11_client/
│   └── x11_client.c      # Ventana Xlib: captura teclas y envía texto al servidor
├── ia_learner/
│   └── ia_learner.c      # Servidor TCP: bag-of-words + clasificación
└── docs/
    └── deployment.puml   # Diagrama de despliegue PlantUML
```

---

## Requisitos previos

```bash
sudo apt update
sudo apt install gcc make libx11-dev
```

Verificar instalación:
```bash
gcc --version
dpkg -l | grep libx11-dev
```

---

## Compilación

Compilar **en este orden** (el Launcher depende de que x11_client ya exista):

### 1. Servidor IALearner
```bash
cd ia_learner
gcc ia_learner.c -o ia_learner -lpthread -Wall -Wextra -I../include
```

### 2. Cliente X11
```bash
cd x11_client
gcc x11_client.c -o x11_client -lX11 -lpthread -Wall -Wextra -I../include
```

### 3. Launcher
```bash
cd launcher
gcc launcher.c -o launcher -Wall -Wextra -I../include
```

---

## Ejecución

Se necesitan **dos terminales**:

### Terminal 1 — Iniciar el servidor IALearner
```bash
cd ia_learner
./ia_learner 9000
```

Salida esperada:
```
[IALearner] Clasificador final esperando documentos...
[IALearner] Escuchando en puerto 9000... (Ctrl+C para detener)
```

### Terminal 2 — Iniciar el Launcher
```bash
cd launcher
./launcher <n_ventanas> [host] [puerto]

# Ejemplos:
./launcher 3                      # 3 ventanas, IALearner en localhost:9000
./launcher 3 127.0.0.1 9000       # igual, con parámetros explícitos
./launcher 1 192.168.1.100 9000   # IALearner en otra máquina
```

Salida esperada:
```
[Launcher] Agentic-OS — 3 ventana(s) | IALearner 127.0.0.1:9000
[Launcher] Notificado a IALearner: esperando 3 ventana(s)
[Launcher] Ventana 1 lanzada (PID ...)
[Launcher] Ventana 2 lanzada (PID ...)
[Launcher] Ventana 3 lanzada (PID ...)
```

---

## Uso de las ventanas X11

Cada ventana gráfica captura el texto del usuario en tiempo real:

| Acción | Resultado |
|--------|-----------|
| Escribir letras | Se acumulan en pantalla |
| **Espacio** | Envía la palabra al IALearner inmediatamente |
| **Enter** | Marca fin de oración — el IALearner muestra la oración completa |
| **Backspace** | Borra el último carácter |
| **Escape** | Cierra la ventana y termina el documento |

> Las palabras se envían **en tiempo real** al presionar espacio,
> sin necesidad de esperar a que el usuario cierre la ventana.

---

## Diccionarios bag-of-words (en español)

Un documento se clasifica en una clase si al menos **3 palabras** del
diccionario aparecen en él. Si hay empate, gana la clase con mayor
frecuencia total de palabras.

| Clase               | Palabras clave |
|---------------------|----------------|
| Correo electrónico  | gracias, favor, saludos, reunion, adjunto, informacion, actualizar, horario, equipo, proyecto |
| Artículo científico | datos, analisis, resultados, metodo, estudio, modelo, investigacion, sistema, significativo, efecto |
| Reporte técnico     | sistema, datos, red, seguridad, aplicacion, servidor, usuario, rendimiento, servicio, infraestructura |

### Ejemplos de texto para probar cada clase

**Correo electrónico:**
```
favor enviar el adjunto del proyecto al equipo
```

**Artículo científico:**
```
los resultados del analisis muestran datos significativos del modelo
```

**Reporte técnico:**
```
el servidor de red presenta problemas de seguridad en el sistema
```

---

## Tabla de inferencia de tipo de usuario

Una vez que todas las ventanas se cierran, el IALearner determina
el tipo de usuario según los tipos de documentos producidos:

| Tipo de usuario        | Correo | Artículo | Reporte |
|------------------------|:------:|:--------:|:-------:|
| Personal administrativo|   ✓    |          |         |
| Personal técnico       |   ✓    |          |    ✓    |
| Profesor               |   ✓    |    ✓     |         |
| Estudiante             |        |    ✓     |    ✓    |

Si los documentos no encajan en ninguna combinación exacta, el sistema
muestra **Indeterminado**.

---

## Protocolo TCP entre componentes

| Mensaje         | Enviado por  | Cuándo                              |
|-----------------|-------------|--------------------------------------|
| `TOTAL <n>\n`   | Launcher    | Antes de lanzar las ventanas         |
| `ID <n>\n`      | x11_client  | Al conectar (primer mensaje)         |
| `WORD <texto>\n`| x11_client  | Al presionar espacio                 |
| `NL\n`          | x11_client  | Al presionar Enter (fin de oración)  |
| Cierre socket   | x11_client  | Al presionar Escape (fin de ventana) |

---

## Parámetros configurables

Todos los valores por defecto están en `include/common.h`.
No hay valores quemados en el código.

| Constante           | Valor     | Descripción                         |
|---------------------|-----------|--------------------------------------|
| `IALEARNER_PORT`    | 9000      | Puerto del servidor                  |
| `IALEARNER_HOST`    | 127.0.0.1 | IP por defecto del servidor          |
| `MAX_WINDOWS`       | 16        | Máximo de ventanas simultáneas       |
| `MAX_SENTENCE_LEN`  | 1024      | Largo máximo de una oración          |
| `MIN_DICT_MATCHES`  | 3         | Mínimo de palabras para clasificar   |
| `NUM_DOC_CLASSES`   | 3         | Número de clases de documento        |

---

## Arquitectura del sistema

```
┌─────────────────────────────────┐      ┌──────────────────────────┐
│        AGENTIC OS               │      │      DATA CENTER          │
│                                 │      │                           │
│  ┌──────────┐                   │      │  ┌────────────────────┐   │
│  │ Launcher │ fork/exec + SIGCHLD│      │  │    IALearner       │   │
│  │ (consola)│──────────────────►│      │  │                    │   │
│  └──────────┘                   │      │  │  hilo por ventana  │   │
│       │                         │ TCP  │  │  BagOfWords + mutex│   │
│       ├──► Ventana X11 #1 ──────┼─────►│  │  classify_document │   │
│       ├──► Ventana X11 #2 ──────┼─────►│  │  infer_user_type   │   │
│       └──► Ventana X11 #N ──────┼─────►│  └────────────────────┘   │
│                                 │      │                           │
└─────────────────────────────────┘      └──────────────────────────┘

Flujo de datos:
  Launcher ──(TOTAL n)──────────────────────────► IALearner
  x11_client ──(ID + WORD por espacio + NL)──────► IALearner
  IALearner ──(resultado en pantalla)
```

---

## Checklist de criterios de evaluación

- [x] Compila sin errores ni warnings con `gcc -Wall -Wextra`
- [x] Sin parámetros quemados — todo configurable en `common.h`
- [x] Sin zombies — `SIGCHLD` + `waitpid(-1, WNOHANG)` en loop
- [x] Sin huérfanos — `prctl(PR_SET_PDEATHSIG, SIGTERM)` en cada hijo
- [x] Sin busy-wait — `pthread_cond_wait` en lugar de loops activos
- [x] `SO_RCVTIMEO` solo en `accept()`, removido de sockets de cliente
- [x] Mutex por documento + mutex de tabla + mutex de consola
- [x] Recursos liberados al terminar (sockets, mutexes, X11, memoria)
- [x] Parámetros inválidos muestran error sin terminar abruptamente
- [x] TDAs apropiados: `BagOfWords`, `DocRecord`, `DocTable`, `Queue`
- [x] Multiprocesamiento — `fork/exec` para clientes X11
- [x] Concurrencia — Pthreads en IALearner (un hilo por conexión)
- [x] IPC remoto — TCP sockets con protocolo propio
- [x] IPC local — señales POSIX (`SIGCHLD`, `SIGTERM`, `SIGPIPE`)
- [x] Envío asíncrono — cola productor-consumidor con mutex+condvar

---

## Solución de problemas frecuentes

**"Cannot open display"** al lanzar ventanas:
```bash
export DISPLAY=:0
```

**"No se encontró ../x11_client/x11_client"**:
Asegúrate de compilar el cliente X11 antes de correr el launcher.

**Puerto ya en uso**:
```bash
fuser -k 9000/tcp
```

**El servidor no cierra con Ctrl+C**:
El servidor tiene un timeout de 1 segundo en `accept()` — espera
hasta 1 segundo antes de verificar la señal y cerrar limpiamente.