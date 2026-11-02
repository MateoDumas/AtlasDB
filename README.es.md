# AtlasDB

*[Read in English](README.md)*

Base de datos relacional pequeña escrita en C++20: almacenamiento persistente en páginas,
lenguaje de consultas inspirado en SQL, índices B+tree, transacciones, recuperación mediante
WAL, restricciones de integridad, una CLI y un **protocolo de red propio** para usarla desde
aplicaciones externas.

```
atlas> CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INT CHECK (age >= 0));
atlas> INSERT INTO users VALUES (1, 'Ana', 31), (2, 'Bruno', 27);
atlas> EXPLAIN SELECT name FROM users WHERE id = 2;
| Project [users.name]                                   |
|   -> IndexScan users USING users_pkey (id = 2)          |
```

## Compilar

Requisitos: CMake ≥ 3.20, un compilador C++20 (GCC 11+, Clang 14+, MSVC 2022) y acceso a
internet la primera vez (CMake descarga Catch2 v3.7.1 y Google Benchmark v1.9.1 con `FetchContent`).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

Opciones: `-DATLAS_BUILD_TESTS=OFF`, `-DATLAS_BUILD_BENCHMARKS=OFF`, `-DATLAS_BUILD_EXAMPLES=OFF`.
Con MinGW los ejecutables se enlazan estáticamente, así que funcionan fuera de MSYS2.

Ejecutables generados:

| Ejecutable        | Qué es                                                        |
|-------------------|---------------------------------------------------------------|
| `atlasdb`         | Shell interactiva (modo embebido o cliente de red)            |
| `atlasdb-server`  | Servidor TCP que habla el protocolo AWP                       |
| `client_example`  | Ejemplo de aplicación externa en C++ usando la librería cliente |
| `atlas_tests`     | Suite de tests (Catch2)                                       |
| `atlas_bench`     | Benchmarks (Google Benchmark)                                 |

El proyecto se distribuye bajo la licencia [MIT](LICENSE). El CI ([.github/workflows/ci.yml](.github/workflows/ci.yml))
compila y corre los tests en Linux (GCC y Clang), macOS y Windows, más jobs con AddressSanitizer,
UndefinedBehaviorSanitizer y ThreadSanitizer.

## Uso

### CLI embebida

```bash
./build/atlasdb mis_datos
```

Abre (o crea) el directorio `mis_datos`. Las sentencias terminan en `;`. Comandos especiales:
`.tables`, `.schema TABLA`, `.read archivo.sql`, `.timer on`, `.help`, `.quit`.
También acepta `-c "SQL"`, `-f script.sql` o SQL por stdin.

### Servidor + clientes

```bash
./build/atlasdb-server --data mis_datos --port 5480
```

```bash
./build/atlasdb --connect 127.0.0.1:5480
```

```bash
python examples/python_client.py 127.0.0.1 5480
```

El cliente Python también soporta parámetros: `db.query("SELECT * FROM t WHERE id = ?", [1])`
y `db.prepare("INSERT INTO t VALUES ($1, $2)").execute(1, "x")`.

Opciones del servidor: `--host`, `--port`, `--max-connections`, `--idle-txn-timeout SEG`
(revierte transacciones inactivas; 60 por defecto, 0 lo desactiva), `--no-sync` (no hace fsync
por commit: más rápido, menos durable). Ctrl+C detiene el servidor ordenadamente y hace checkpoint.

### Desde una aplicación C++

```cpp
#include "net/client.h"

auto db = atlas::Client::connect("127.0.0.1", 5480, "mi-app");

// Parámetros: los valores viajan aparte del SQL, así que no hay inyección posible.
auto r = db.query("SELECT nombre FROM clientes WHERE ciudad = $1 AND edad > $2",
                  atlas::params("Córdoba", 30));

// Sentencia preparada: se parsea una vez y se ejecuta muchas veces.
auto mover = db.prepare("UPDATE cuentas SET saldo = saldo + ? WHERE id = ?");
db.query("BEGIN");
mover.execute(atlas::params(-10, 1));
mover.execute(atlas::params(10, 2));
db.query("COMMIT");
```

`atlas::params(...)` acepta enteros, reales, `bool`, strings y `nullptr` (NULL).

Los errores llegan como `atlas::DbError` con el código del servidor (`constraint_violation`,
`syntax_error`, `locked`, ...). También se puede embeber el motor sin red con
`atlas::Database::open(dir)` + `atlas::Session`.

## SQL soportado

| Categoría      | Sintaxis |
|----------------|----------|
| Tipos          | `INTEGER`/`INT`/`BIGINT`, `REAL`/`FLOAT`/`DOUBLE`/`NUMERIC`, `TEXT`/`VARCHAR(n)`, `BOOLEAN` |
| DDL            | `CREATE TABLE [IF NOT EXISTS]`, `DROP TABLE [IF EXISTS]`, `CREATE [UNIQUE] INDEX nombre ON t (col)`, `DROP INDEX` |
| Restricciones  | `PRIMARY KEY`, `NOT NULL`, `UNIQUE`, `DEFAULT expr`, `CHECK (expr)` (de columna o tabla), `REFERENCES t(col)` / `FOREIGN KEY (c) REFERENCES t(col)` |
| DML            | `INSERT INTO t [(cols)] VALUES (...), (...)`, `INSERT INTO t SELECT ...`, `UPDATE t SET c = expr [WHERE]`, `DELETE FROM t [WHERE]` |
| Consultas      | `SELECT [DISTINCT] ... FROM t [alias]`, `[INNER] JOIN ... ON`, `LEFT [OUTER] JOIN`, `CROSS JOIN` / coma, `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY expr|alias|posición [ASC|DESC]`, `LIMIT n [OFFSET m]` |
| Expresiones    | `+ - * / %`, `||`, `= != <> < <= > >=`, `AND OR NOT`, `IS [NOT] NULL`, `[NOT] IN (...)`, `[NOT] BETWEEN`, `[NOT] LIKE` (`%`, `_`) con lógica de tres valores |
| Funciones      | `COUNT(*)`, `COUNT([DISTINCT] x)`, `SUM`, `AVG`, `MIN`, `MAX`, `UPPER`, `LOWER`, `LENGTH`, `ABS`, `ROUND`, `SUBSTR`, `COALESCE`/`IFNULL`, `TYPEOF` |
| Transacciones  | `BEGIN`, `COMMIT`, `ROLLBACK` |
| Parámetros     | `$1`, `$2`, ... o `?` (posicionales) en cualquier expresión; no se pueden mezclar ambos estilos en una sentencia |
| Utilidades     | `EXPLAIN SELECT/UPDATE/DELETE`, `SHOW TABLES`, `DESCRIBE t`, `CHECKPOINT` |

## Arquitectura

```
 SQL ──► Lexer ──► Parser ──► AST ──► Planner ──► árbol de operadores (Volcano) ──► resultados
                                         │                   │
                                         │                   ▼
                                         │      Table: heap de páginas + índices B+tree
                                         ▼                   │
                         Database: catálogo, restricciones,  ▼
                         transacciones, WAL, checkpoint   Pager (buffer pool) ──► archivos .tbl
```

| Módulo | Archivos | Responsabilidad |
|--------|----------|-----------------|
| Lexer y parser | `src/sql/` | Tokenización con posiciones, parser descendente recursivo con precedencia de operadores, errores con línea/columna |
| Representación de consultas | `src/sql/ast.h` | AST de sentencias y expresiones; las expresiones se pueden serializar a SQL (se usa para guardar `DEFAULT`/`CHECK`) |
| Planificador | `src/engine/planner.cpp` | Expansión de `*`, resolución de columnas, empuje de predicados a cada tabla, elección de índice (igualdad, `IN`, rangos y `BETWEEN`), index nested-loop join cuando el `ON` es `a = b.col_indexada`, agregación, `ORDER BY` por alias/posición/expresión oculta |
| Ejecutor | `src/engine/operators.cpp`, `session.cpp` | Operadores pull: SeqScan, IndexScan, Filter, NestedLoopJoin, IndexJoin, Aggregate, Project, Distinct, Sort, Limit. `Session` ejecuta sentencias y maneja autocommit/transacciones |
| Almacenamiento | `src/storage/` | Páginas de 4 KiB con slots, buffer pool LRU por tabla, heap con mapa de espacio libre, WAL con CRC32. Las filas de más de 2 KB se parten en *chunks* encadenados detrás de un puntero (hasta 16 MiB por fila) |
| Índices | `src/index/` | B+tree propio (split, préstamo y fusión; hojas encadenadas para rangos) con claves `(valor, RID)`, así que soporta duplicados. Cada índice vive en su propio archivo; los nodos se cargan del disco la primera vez que se usan |
| Transacciones | `src/txn/`, `src/engine/database.cpp` | Undo log en memoria, redo log escrito al hacer commit, savepoints por sentencia, lock global de transacción |
| Red | `src/net/` | Sockets multiplataforma, protocolo AWP, servidor (un hilo por conexión) y librería cliente |

### Archivos en disco

```
mis_datos/
  catalog.bin   esquemas, índices explícitos, LSN del último checkpoint (escrito atómicamente + CRC)
  t<id>.tbl     páginas de cada tabla (cada página guarda el LSN del último cambio y un CRC32)
  i<id>.idx     nodos de cada índice B+tree (página 0: raíz, cantidad de claves, páginas libres)
  wal.log       log de redo desde el último checkpoint
  doublewrite.buf  sólo existe durante un checkpoint (ver abajo)
  LOCK          impide que dos procesos abran el mismo directorio
```

### Transacciones y recuperación

- **Aislamiento serializable** mediante un lock lector/escritor de base de datos:
  - Las lecturas en autocommit (`SELECT`, `EXPLAIN`, `SHOW TABLES`, `DESCRIBE`) toman el lock
    compartido y corren en paralelo entre sí.
  - Las escrituras, el DDL y las transacciones explícitas (de `BEGIN` a `COMMIT`/`ROLLBACK`)
    toman el lock exclusivo.
  - Un escritor en espera frena a los lectores nuevos, así que no puede quedar esperando para
    siempre.
  - Quien no consigue el lock en `lock_timeout` (5 s) recibe el error `locked`.
- **Timeout de transacciones inactivas** (servidor): si un cliente deja una transacción abierta
  sin enviar nada durante `--idle-txn-timeout` segundos (60 por defecto), el servidor la revierte,
  le envía un error `transaction_error` y cierra la conexión, liberando el lock para los demás.
- **Atomicidad por sentencia**: si falla una sentencia dentro de una transacción (por ejemplo
  la tercera fila de un `INSERT` viola una clave), sólo esa sentencia se deshace y la
  transacción sigue abierta. En autocommit se deshace la sentencia completa.
- **WAL redo-only + no-steal**: las páginas modificadas no se escriben al archivo de datos hasta
  un checkpoint, que sólo ocurre sin transacciones activas. Por eso nunca hay datos sin
  confirmar en disco y la recuperación sólo necesita rehacer. Cada `COMMIT` escribe su lote de
  registros más un registro `Commit`, **libera el lock y recién entonces espera el `fsync`**: el
  primero que espera hace un `fsync` que cubre todos los commits escritos hasta ese momento
  (*group commit*), y una lectura sólo devuelve resultados cuando lo que vio ya es durable.
- **Checkpoint atómico con doublewrite buffer**: al superar 8 MiB de WAL, con `CHECKPOINT` o al
  cerrar. Los nodos de índice modificados se serializan en sus páginas. Luego todas las páginas
  sucias, junto con el catálogo nuevo, se escriben primero en `doublewrite.buf` (con CRC) y se
  hace `fsync`. Recién entonces se escriben en su lugar, se reemplaza `catalog.bin` y se vacía el
  WAL. Si el proceso cae mientras escribe las páginas en su lugar, al abrir se copian de nuevo
  desde el buffer. Así los archivos de datos e índices siempre están exactamente en el estado de
  algún checkpoint, aunque una página haya quedado escrita a medias.
- **Recuperación al abrir**: se repara desde el doublewrite buffer si hace falta y se carga el
  catálogo. Después se rehacen las transacciones del WAL con `Commit` cuyo LSN supera el del
  checkpoint (un final cortado o con CRC inválido se descarta). Cada cambio de fila se aplica
  también a los índices persistentes, así que **no hay que reconstruir nada**: abrir la base no
  depende del tamaño de los datos.
- **Checksums de página**: cada página lleva un CRC32 que se calcula al escribirla y se verifica
  al leerla. Una página corrupta se reporta como error `corruption` en vez de devolver datos
  incorrectos.

Los tests de `tests/test_recovery.cpp` simulan caídas (sin checkpoint), finales de WAL cortados,
checkpoints interrumpidos con páginas de datos e índices escritas a medias, y una carga aleatoria
con seis caídas sucesivas comparada contra un modelo en memoria.

### Restricciones de integridad

`PRIMARY KEY` (implica `NOT NULL` y un índice único `<tabla>_pkey`), `UNIQUE` (permite varios
NULL), `NOT NULL`, `CHECK` (NULL lo satisface, como en SQL estándar), `VARCHAR(n)`, tipos
estrictos (sólo se convierte INTEGER↔REAL sin pérdida) y `FOREIGN KEY` con semántica
`RESTRICT`: no se puede insertar un hijo huérfano, ni borrar/cambiar una clave padre
referenciada, ni borrar una tabla referenciada. Se admiten referencias a la misma tabla.

## Protocolo AtlasDB Wire Protocol (AWP v1)

Cada mensaje es `[u8 tipo][u32 longitud big-endian][cuerpo]`. La especificación completa está en
[src/net/protocol.h](src/net/protocol.h).

| Dirección | Tipo | Contenido |
|-----------|------|-----------|
| C → S | `H` Hello | `"AWP1"`, u16 versión, string nombre del cliente |
| C → S | `Q` Query | string SQL (una o varias sentencias) |
| C → S | `P` Prepare | string nombre, string SQL (una sentencia con `$n` o `?`) |
| C → S | `B` Execute | string nombre, u16 cantidad, valores de los parámetros |
| C → S | `D` Close | string nombre (libera la sentencia preparada) |
| C → S | `X` Terminate | vacío |
| S → C | `O` HelloOk | u16 versión, string nombre del servidor |
| S → C | `R` RowSet | columnas (nombre + tipo), filas de valores etiquetados, tag |
| S → C | `C` Complete | tag (`INSERT 3`), u64 filas afectadas |
| S → C | `p` Prepared | u16 cantidad de parámetros que espera la sentencia |
| S → C | `E` Error | string código, string mensaje |
| S → C | `Z` Ready | `'I'` sin transacción / `'T'` dentro de una transacción |

Por cada `Query` el servidor responde un `R`/`C` por sentencia, se detiene en el primer `E` y
siempre termina con `Z`. `Prepare`, `Execute` y `Close` también terminan con `Z`. Las sentencias
preparadas pertenecen a la conexión. El nombre vacío (`""`) es la sentencia "sin nombre": los
clientes la usan para ejecutar una consulta con parámetros en un solo viaje de ida y vuelta,
enviando `P` y `B` seguidos sin esperar la respuesta del primero. Si un cliente se desconecta con una transacción abierta, se revierte.
[examples/python_client.py](examples/python_client.py) implementa un cliente completo en ~150
líneas de Python sin dependencias.

## Benchmarks

`./build/atlas_bench` (Release, MinGW GCC 15, Windows 11, 8 hilos a 2.9 GHz; resultados orientativos):

| Benchmark | Resultado |
|-----------|-----------|
| B+tree: inserción aleatoria (100k claves) | ~9 M claves/s |
| B+tree: búsqueda (1M claves) | ~250 ns |
| Parseo de un SELECT con JOIN/GROUP BY | ~5 µs |
| INSERT durables (`fsync`) con 1 / 4 / 16 clientes concurrentes | ~580 / ~1.250 / ~3.860 por segundo (group commit) |
| INSERT en autocommit sin `fsync` (`--no-sync`) | ~64k/s |
| `ORDER BY id DESC LIMIT 10` sobre 100k filas (orden del índice) | ~37 µs |
| 10.000 INSERT en una transacción | ~48 ms |
| SELECT por clave primaria (100k filas) | ~10 µs |
| El mismo SELECT como sentencia preparada | ~7.5 µs |
| Misma búsqueda sin índice (10k filas, full scan) | ~3.3 ms |
| GROUP BY sobre 100k filas | ~52 ms |
| JOIN con índice 20k × 10k + GROUP BY | ~25 ms |
| Abrir una base de 100k filas y 2 índices + primera consulta | ~18 ms |
| Lo que costaba antes reconstruir **un** índice de 100k filas al abrir | ~90 ms |

## Limitaciones conocidas

- Las lecturas corren en paralelo, pero hay un solo escritor a la vez y las escrituras no
  corren junto con las lecturas. No hay MVCC ni locks por fila. El timeout de transacciones
  inactivas sólo existe en el servidor: con la API embebida, una `Session` con una transacción
  abierta bloquea a las demás hasta que termine.
- Los nodos de índice modificados no se pueden desalojar hasta el siguiente checkpoint, así que
  una transacción que toca muchísimas claves usa memoria proporcional a esos cambios.
- Índices, claves primarias y foráneas de una sola columna. No hay `ALTER TABLE`, subconsultas,
  `UNION` ni vistas.
- Política no-steal: una transacción enorme mantiene todas sus páginas sucias en memoria.
- El doublewrite buffer repara páginas dañadas por una caída durante un checkpoint. Una
  corrupción fuera de ese caso (por ejemplo, un disco que falla) se detecta pero no se repara.
- En `ORDER BY`, los NULL van primero en orden ascendente.

## Estructura del repositorio

```
src/common    valores, serialización, CRC32, formato de resultados
src/sql       lexer, AST, parser
src/catalog   esquemas de tablas e índices
src/storage   archivos, páginas, buffer pool, heap, WAL
src/index     B+tree e índices
src/txn       lock de transacciones y estado de transacción
src/engine    expresiones, operadores, planificador, base de datos, sesiones
src/net       sockets, protocolo, servidor, cliente
apps/         atlasdb (CLI) y atlasdb-server
examples/     cliente C++ y cliente Python
tests/        tests Catch2 (63 casos)
bench/        benchmarks Google Benchmark
```
