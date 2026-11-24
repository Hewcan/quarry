# Arquitectura de Quarry

Quarry es un gestor de bases de datos de escritorio, nativo y multiplataforma. Sus prioridades, por orden: **1) ligereza y rendimiento**, **2) interfaz moderna y accesible (WCAG AA, teclado, lectores de pantalla)**, **3) facilidad para añadir motores mediante plugins**.

## 1. Decisiones cerradas

| Tema | Decisión |
|---|---|
| Nombre | **Quarry**: namespace `quarry`, binario `quarry`, ID `dev.quarry.Quarry` |
| Licencia | Apache-2.0 (código propio). Qt LGPLv3 enlazado dinámicamente |
| Plataformas mínimas | Windows 10 1809+ / 11 (x64), macOS 14.4+ (arm64 + x86_64), Linux x64 con glibc ≥ 2.39 (Ubuntu 24.04+, Debian 13+, Fedora 40+). Es el suelo de los binarios de Qt 6.12; Qt 6.12 es la última versión que soporta Windows 10 |
| Qt | 6.12 LTS (`.qt-version`) instalado con aqt. Módulos: Core, Gui, Qml, Quick, QuickControls2, ShaderTools; en Linux también DBus (lo pide QtKeychain) |
| Motores | Hito 1: SQLite (compilado dentro) y PostgreSQL (libpq). Después: MySQL/MariaDB (MariaDB Connector/C) y SQL Server/ODBC genérico. Oracle (ODPI-C) sin fecha |
| Tests | Qt Test (C++) + Qt Quick Test (QML) + `QAbstractItemModelTester` |
| Plugins | Plugins Qt (`QPluginLoader` + `Q_PLUGIN_METADATA`), cargados solo al usarse |
| Persistencia propia | SQLite de la app (`quarry.db`) para conexiones, historial y estado; preferencias simples en `QSettings` |
| Idiomas | Textos fuente en inglés (`qsTr`/`tr`) + traducción al español (`.ts`) |
| Estilo visual | Estilo propio sobre Qt Quick Controls **Basic**, con tokens de tema |
| Iconos | Lucide (ISC) como fuente recortada; la copia original fijada está en `third_party/lucide/` (lucide-static 1.72.0) |
| Telemetría | Ninguna. Sin conexiones salientes salvo las que el usuario abre hacia sus bases de datos (y, si el usuario activa la IA de H13 en una conexión, las que hace su propio Claude Code; la app no abre ninguna, §6.13) |
| Dependencias | vcpkg (manifiesto). Las librerías que dependen de Qt (QtKeychain, KSyntaxHighlighting) se compilan contra la Qt de aqt con overlay ports *shim* (§9) |

## 2. Capas y módulos

```
┌──────────────────────────── app (ejecutable quarry) ────────────────────────────┐
│ QML (vistas, componentes del estilo propio)                                     │
│ ViewModels / modelos C++: ConnectionListModel, SchemaTreeModel, ResultTableModel│
│ ThemeManager, CommandRegistry, AccessibleGrid (QAccessibleTableInterface)        │
└───────────────▲────────────────────────────────────────────────────────────────┘
                │ señales/slots (Qt::QueuedConnection) — solo tipos de core
┌───────────────┴──────────── core (quarry_core, solo Qt6::Core) ────────────────┐
│ Interfaces de provider · ProviderRegistry · Session/QueryRunner (hilos)        │
│ ResultBatch (columnar) · Result<T>/Error · SqlSafety · ConnectionStore (SQLite)│
│ CredentialStore (QtKeychain) · AppDatabase (migraciones)                        │
└───────────────▲────────────────────────────────────────────────────────────────┘
                │ IProviderPlugin (QPluginLoader, carga diferida)
┌───────────────┴──────────── providers/* (un plugin MODULE por motor) ──────────┐
│ sqlite (sqlite3 dentro)   postgres (libpq)   mariadb   odbc   …                 │
└────────────────────────────────────────────────────────────────────────────────┘
```

Reglas de dependencia (las verifica CMake: cada target enlaza solo lo permitido):
- `quarry_core` enlaza solo `Qt6::Core`, `sqlite3` (base de datos de la app) y `qt6keychain`. Nunca Gui/Qml/Quick.
- `providers/*` enlazan `quarry_core` (solo los headers de la API de provider, `quarry_provider_api`) y su driver. Nunca `app`.
- `app` enlaza `quarry_core` y Qt Quick. **Nunca enlaza un provider**: solo los conoce por `ProviderRegistry`.

### Estructura de carpetas

```
src/
  core/            quarry_core: include/quarry/… (API pública), src/…
  core/api/        quarry_provider_api: headers de la interfaz de provider (INTERFACE lib)
  ai/              quarry_ai: integración de Claude Code, solo Qt6::Core (§6.13)
  providers/
    sqlite/        quarry_provider_sqlite (MODULE)
    postgres/      quarry_provider_postgres (MODULE)
  app/             ejecutable quarry: main.cpp, viewmodels/, qml/, i18n/
    qml/Quarry/Controls/   componentes del estilo propio (QButton, QTextField, …)
    qml/Quarry/Views/      pantallas
tests/
  core/  providers/  app/ (Qt Quick Test)  fixtures/
tools/
  env-check/  bench/
cmake/  scripts/  docs/  vcpkg-overlays/{ports,triplets}/
```

## 3. Tipos base de `core`

C++20 no tiene `std::expected` (llega en C++23), así que `core` define un `quarry::Result<T>` mínimo con la misma forma (`has_value()`, `value()`, `error()`, `operator bool`), para poder sustituirlo por `std::expected` sin tocar los usos. **No se lanzan excepciones a través de la frontera del plugin.**

```cpp
namespace quarry {

enum class ErrorKind : uint8_t {
    Connection, Authentication, Timeout, Cancelled, Syntax, Permission,
    Constraint, NotFound, Unsupported, Driver, Internal
};

struct Error {
    ErrorKind kind = ErrorKind::Internal;
    QString message;            // lenguaje claro, traducible: "Wrong user name or password"
    QString detail;             // técnico, desplegable: texto literal del driver
    QString sqlState;           // "28P01", si existe
    std::optional<int> position;// posición en el SQL (para subrayar en el editor)
};

template <typename T> class Result;   // ≈ std::expected<T, Error>

// Cancelación cooperativa: la consultan los bucles largos dentro del provider.
class CancellationToken {
public:
    bool isCancelled() const noexcept;   // std::atomic<bool>, sin bloqueo
};
}
```

### Resultados: datos crudos en columnas

`ResultBatch` guarda N filas en formato **columnar**. Nada se convierte a texto hasta pintar la celda.

```cpp
namespace quarry {

enum class ValueType : uint8_t { Null, Bool, Int64, Double, Decimal, Text, Bytes,
                                 Date, Time, DateTime, DateTimeTz, Uuid, Json, Other };

struct ColumnInfo {
    QString name;
    ValueType type;          // tipo lógico normalizado
    QString nativeType;      // "int4", "varchar(255)", …
    bool nullable;
    std::optional<ObjectPath> sourceTable; // necesario para la edición (Hito 2)
};

class ColumnBuffer {          // un buffer por columna
    // fijos: std::vector<int64_t>/<double>/…; variables: arena de bytes UTF-8 + offsets (uint32)
    // nulls: bitmap
public:
    bool isNull(qsizetype row) const;
    int64_t int64At(qsizetype row) const;
    double doubleAt(qsizetype row) const;
    QByteArrayView bytesAt(qsizetype row) const; // Text/Decimal/Json/Bytes/Other: vista sin copia
};

struct ResultBatch {
    qsizetype rowCount = 0;
    std::vector<ColumnBuffer> columns;    // columns.size() == número de columnas
};
}
```

**Columnas heterogéneas** (decisión del 2026-11-12): un `ColumnBuffer` empieza tipado (solo el almacén de su tipo + bitmap de NULLs). Si llega un valor de otro tipo (en SQLite el tipo es por valor; también pasa con `UNION`, expresiones o columnas sin tipo declarado), el buffer pasa a **modo mixto**: reserva un vector de 1 byte por fila con el `ValueType` de cada celda y guarda cada valor en el almacén de su tipo (int64, double o bytes). No se pierde fidelidad (un BLOB sigue siendo BLOB) y solo cuesta +1 B/celda en las columnas mezcladas. `ColumnInfo::type` de una columna mixta vale `ValueType::Other`; el tipo real de cada celda se consulta con `ColumnBuffer::typeAt(row)`.

Motivo: con 1 M de filas, un `QVariant` por celda costaría unos 16 B más una reserva de memoria por string. En columnas, un `int64` ocupa 8 B, y los textos van a una arena contigua con un offset de 4 B por celda. `ResultTableModel::data()` convierte solo las celdas visibles, y para eso consulta el `ValueFormatter` del dialecto.

## 4. Interfaz de provider

Los providers implementan una **API síncrona y bloqueante**, siempre llamada desde un hilo de trabajo que gestiona `core`. Así escribir un plugin es sencillo: el autor no gestiona hilos ni event loops. La asincronía, el envío por lotes y la cancelación los resuelve `core` (§5). La única llamada que se hace desde otro hilo es `requestCancel()`, que debe ser thread-safe.

```cpp
namespace quarry {

struct ProviderInfo {
    QString id;                 // "postgres"
    QString displayName;        // "PostgreSQL"
    QVersionNumber version;
    Capabilities caps;          // flags: Schemas, MultipleDatabases, Transactions, Cancel,
                                //        Explain, ServerSideCursor, MultipleResultSets, Ssh, …
};

// Describe el formulario de conexión; app lo genera sin conocer el motor.
struct ConnectionField {
    QString key;                // "host", "port", "database", "file", "sslmode"…
    QString label;              // traducible
    enum class Kind { Text, Number, Path, Enum, Bool, Secret } kind;
    QVariant defaultValue;
    QStringList enumValues;
    bool required = false;
    bool advanced = false;      // se oculta en "Opciones avanzadas"
};

struct ConnectionConfig {
    QUuid id;
    QVariantMap values;         // sin secretos
};

struct Secrets {                // viene de CredentialStore, no se persiste
    QString password;
};

// Identifica un objeto del árbol: [database, schema, table, …] + tipo.
struct ObjectPath { QStringList parts; ObjectKind kind; };

enum class ObjectKind : uint8_t { Database, Schema, Table, View, MaterializedView,
                                  Column, Index, ForeignKey, Function, Procedure,
                                  Sequence, Trigger, Folder };

struct SchemaNode {
    ObjectPath path;
    QString name;
    bool hasChildren;           // pinta el triángulo sin cargar los hijos
    QString detail;             // "int4 NOT NULL", "≈ 12 345 rows"…
};

struct TableDetails {
    std::vector<ColumnDef> columns;
    std::vector<IndexDef> indexes;
    std::vector<ForeignKeyDef> foreignKeys;
    std::optional<QStringList> primaryKey;
};

struct Statement {
    QString sql;
    QVariantList parameters;    // posicionales; cada dialecto traduce los placeholders
    int fetchSize = 500;
};

class IResultStream {
public:
    virtual ~IResultStream() = default;
    virtual const std::vector<ColumnInfo>& columns() const = 0;   // vacío si no devuelve filas
    // Bloquea hasta tener maxRows filas o llegar al final. Un batch vacío con atEnd() == true marca el fin.
    virtual Result<ResultBatch> fetch(qsizetype maxRows, const CancellationToken&) = 0;
    virtual bool atEnd() const = 0;
    virtual std::optional<qint64> affectedRows() const = 0;       // UPDATE/DELETE/INSERT
    virtual QStringList notices() const = 0;                      // RAISE NOTICE, warnings
    // Varios result sets (SQL Server, MySQL CALL…). Devuelve false si no hay más.
    virtual Result<bool> nextResult(const CancellationToken&) = 0;
};

class IConnection {
public:
    virtual ~IConnection() = default;
    virtual Result<ServerInfo> open(const CancellationToken&) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    // Introspección bajo demanda: solo los hijos directos de `parent`.
    virtual Result<std::vector<SchemaNode>> children(const ObjectPath& parent,
                                                     const CancellationToken&) = 0;
    virtual Result<TableDetails> describe(const ObjectPath& table, const CancellationToken&) = 0;

    // Ejecución en streaming. El stream tiene la conexión ocupada hasta que se destruye.
    virtual Result<std::unique_ptr<IResultStream>> execute(const Statement&,
                                                           const CancellationToken&) = 0;

    // Thread-safe: se llama desde el hilo de UI mientras otro hilo está en execute()/fetch().
    // Debe hacer que la llamada bloqueada vuelva pronto con ErrorKind::Cancelled.
    virtual void requestCancel() = 0;

    // Transacciones explícitas (Hito 2, edición de datos)
    virtual Result<void> begin() = 0;
    virtual Result<void> commit() = 0;
    virtual Result<void> rollback() = 0;
};

class ISqlDialect {
public:
    virtual ~ISqlDialect() = default;
    virtual QString quoteIdentifier(QStringView) const = 0;      // "a""b" | `a``b` | [a]]b]
    virtual QString quoteLiteral(QStringView) const = 0;
    virtual QString highlightingDefinition() const = 0;          // "SQL (PostgreSQL)" de KSyntaxHighlighting
    virtual QStringList keywords() const = 0;                    // para autocompletado
    // Divide un script en sentencias: tiene en cuenta $$…$$, DELIMITER, comentarios y strings.
    virtual std::vector<StatementRange> splitStatements(QStringView script) const = 0;
    virtual QString limitClause(qint64 limit, qint64 offset) const = 0;
    virtual QString explainPrefix(ExplainMode) const = 0;        // Hito EXPLAIN
};

class IDatabaseProvider {
public:
    virtual ~IDatabaseProvider() = default;
    virtual ProviderInfo info() const = 0;
    virtual std::vector<ConnectionField> connectionFields() const = 0;
    virtual const ISqlDialect& dialect() const = 0;
    // Crea una conexión sin abrirla. No toca la red.
    virtual std::unique_ptr<IConnection> createConnection(const ConnectionConfig&,
                                                          const Secrets&) const = 0;
};

// Punto de entrada del plugin (QObject + Q_PLUGIN_METADATA)
class IProviderPlugin {
public:
    virtual ~IProviderPlugin() = default;
    virtual std::unique_ptr<IDatabaseProvider> createProvider() = 0;
};
}
#define QUARRY_PROVIDER_IID "dev.quarry.IProviderPlugin/1"
Q_DECLARE_INTERFACE(quarry::IProviderPlugin, QUARRY_PROVIDER_IID)
```

Prueba de conexión = `createConnection()` + `open()` + `close()`, con un timeout que aplica `core`.

**Ajustes de implementación (H1.1):** el código manda; esto resume en qué se aparta de las firmas de arriba.
- `ObjectKind`/`ObjectPath` están en `object_path.h` (los usan tanto `ColumnInfo` como la API de provider). `Capabilities` es `QFlags<Capability>`.
- `ColumnBuffer` guarda int64, bool y double en el mismo slot de 8 bytes (el double como patrón de bits), así que el modo mixto solo añade la etiqueta de 1 B por fila. `appendBytes()` devuelve `false` y guarda NULL si la arena del batch superase 4 GiB.
- Los errores se devuelven con `quarry::fail(Error)`. Un acceso indebido a `Result` (p. ej. `value()` sin valor) termina con `qFatal`, también en release.
- **`splitStatements`:** devuelve rangos semiabiertos `[begin, end)` en unidades UTF-16 sobre el texto original, sin espacios alrededor y **sin el `;` final**. Las sentencias vacías o que solo tienen comentarios se descartan. Todos los providers siguen esta convención (`tests/providers/conformance`).
- Una columna sin tipo declarado (expresiones, `UNION`) tiene `ColumnInfo::type == ValueType::Other`. En SQLite, `DECIMAL`/`NUMERIC` llegan siempre como texto exacto.
- **Contrato de cancelación:** `requestCancel()` es thread-safe y afecta solo a la llamada en curso. Cada `open/children/describe/execute/fetch` descarta al empezar cualquier cancelación pendiente, para que una cancelación tardía nunca aborte la sentencia siguiente.
- **`Session`:** `open()` y `execute()` se pueden encadenar. El `execute` espera en cola a que acabe la apertura y, si la apertura falla, termina con `failed()` ("The connection is not open."). Un `execute` nuevo cancela el `execute`/`fetchMore` en curso, pero nunca la apertura. `fetchMore` se ignora si la sesión está ocupada, y `cancel()` con la sesión inactiva no toca la conexión. `ExecutionStats` acumula tiempo y filas entre `execute` y los `fetchMore` siguientes.

### Cómo cumple cada motor la API

| Motor | Streaming | Cancelación |
|---|---|---|
| SQLite | `sqlite3_step` por fila hasta llenar el batch | `sqlite3_interrupt()` (thread-safe) |
| PostgreSQL | `PQsendQueryParams` + `PQsetChunkedRowsMode(fetchSize)` (libpq ≥ 17) | `PQcancelCreate` + `PQcancelBlocking` sobre una conexión de cancelación aparte |
| MariaDB | `mysql_use_result` + `mysql_fetch_row` | `KILL QUERY <id>` desde una conexión auxiliar |
| ODBC | `SQLFetch` con *row-wise binding* de bloques | `SQLCancel` (thread-safe por especificación) |

La contrapresión es natural: si el usuario no pide más filas, `core` no llama a `fetch()` y el socket deja de vaciarse (control de flujo TCP).

## 5. Modelo de hilos

```
Hilo UI (QGuiApplication)          Hilo de sesión (1 QThread por sesión)
──────────────────────────         ─────────────────────────────────────
QueryController.run(sql) ──Queued──▶ SessionWorker::execute()
                                       IConnection::execute()
                                       stream->fetch(500) ─┐
ResultTableModel.append() ◀─Queued── batchReady(batch)  ◀─┘
  (beginInsertRows/endInsertRows)
fetchMore() ──────────────Queued──▶ SessionWorker::fetchNext(500)
Cancel button ── directo ──────────▶ IConnection::requestCancel() (thread-safe)
                                     + CancellationToken.cancel()
```

- **Sesión** = una `IConnection` abierta + su `QThread` + `SessionWorker` (QObject movido al hilo). El `IConnection` solo se usa desde ese hilo, salvo `requestCancel()`.
- Cada conexión abierta en la interfaz tiene **una sesión de metadatos** (árbol de esquemas) y **una sesión por pestaña de editor**, que se crea al ejecutar por primera vez. Así una consulta larga no congela el árbol. En SQLite las sesiones abren el mismo archivo y cada pestaña tiene su propio `sqlite3*`.
- Los batches viajan por `Qt::QueuedConnection` como `std::shared_ptr<const ResultBatch>` (Q_DECLARE_METATYPE). El hilo de UI no copia datos.
- **Carga bajo demanda:** el primer batch es de 500 filas (configurable). `ResultTableModel::canFetchMore()/fetchMore()` pide el siguiente cuando la vista se acerca al final. *Fetch all* pide batches seguidos y la UI los agrupa en `beginInsertRows` cada ≤ 50 ms, para no saturar el event loop.
- **Límite de memoria:** si el resultado supera un umbral configurable (por defecto 2 M filas o 1 GB), se para el fetch y se avisa al usuario con la opción de seguir o de exportar en streaming.
- **Timeouts:** conexión 15 s (configurable). Las consultas no tienen timeout por defecto; se cancelan manualmente.
- Ninguna llamada de red ni de disco pesada en el hilo de UI. `ProviderRegistry` lee los metadatos de los plugins en el arranque sin cargarlos (§8), y `QPluginLoader::load()` se ejecuta en el hilo de sesión la primera vez que se usa el motor.

### 5.1 Contrato del árbol de esquemas (H1.4)

Ampliación de `core` (cabeceras en `src/core/include/quarry/`) que usa `SchemaTreeModel` (app):

```cpp
namespace quarry {
class ProviderRegistry {
    // Lo que hoy hace ConnectionTester, extraído para reutilizarlo. Carga el plugin si hace falta.
    // Errores: NotFound "This database engine is not available." (detail = motivo del loader);
    //          Internal "Could not prepare the connection." si el provider devuelve nullptr.
    Result<std::unique_ptr<IConnection>> createConnection(const ConnectionProfile &profile,
                                                          const Secrets &secrets);
};

class Session {
public slots:
    // Hijos directos de `parent` (IConnection::children). Se encola en orden con las demás
    // operaciones; no cancela un execute en curso. Antes de que termine open() espera a él;
    // si open() falló -> childrenFailed("The connection is not open."). Devuelve un id > 0.
    quint64 children(const quarry::ObjectPath &parent);
signals:
    void childrenReady(quint64 requestId, quarry::ObjectPath parent,
                       std::vector<quarry::SchemaNode> nodes);
    // También por cancel()/close(): cada petición pendiente o en curso termina con
    // childrenFailed(ErrorKind::Cancelled), para que la UI quite su "Loading…".
    void childrenFailed(quint64 requestId, quarry::ObjectPath parent, quarry::Error error);
};
}
Q_DECLARE_METATYPE(quarry::ObjectPath)
Q_DECLARE_METATYPE(std::vector<quarry::SchemaNode>)
```

- Cada conexión desplegada en el árbol tiene su `Session` de metadatos, propiedad del modelo. Desplegar conecta; plegar **no** desconecta; se desconecta con el comando "Disconnect" o al cerrar la app.
- Contraseña: `Saved` -> `CredentialStore::readPassword`; `AskEveryTime` -> diálogo que guarda la contraseña con `rememberForSession` (solo en memoria hasta cerrar la app); `None` -> sin secretos.
- `ConnectionTester` pasa a usar `ProviderRegistry::createConnection` (mismo comportamiento y mismos mensajes).
- `SchemaTreeModel::fetchMore` solo marca el nodo y encola la petición (cambiar el modelo dentro de `fetchMore` es ilegal cuando lo llama otra señal); la fila "Loading…" aparece en el siguiente ciclo del event loop. Las respuestas se identifican por (sesión, requestId) y las de una sesión cerrada se descartan.
- shortcut: `ProviderRegistry::provider()` carga el plugin en el hilo de UI, no en el de sesión como dice §5; se moverá cuando haya providers con carga lenta (H1.8).

## 6. Flujo de una consulta de principio a fin

1. El usuario pulsa **Ctrl/Cmd+Enter** en el editor. `QueryController` obtiene la sentencia bajo el cursor con `dialect().splitStatements()`.
2. **`SqlSafety`** analiza la sentencia con el tokenizador del dialecto. Si es `UPDATE`/`DELETE` sin `WHERE`, o `DROP`/`TRUNCATE` en una conexión marcada como producción, muestra un diálogo de confirmación con el texto de la sentencia. Escape cancela; para confirmar hay que pulsar un botón explícito, que no es el de por defecto.
3. `QueryController` pide la sesión de la pestaña a `SessionManager`, que la crea si no existe. Si la conexión necesita contraseña, la saca de `CredentialStore` en el hilo de sesión, porque QtKeychain hace E/S.
4. `SessionWorker::execute(Statement)` → `IConnection::execute()`, y después `fetch(500)`.
5. Señales hacia la UI: `started` (el estado pasa a "Running…" y se activa Cancel), `columnsReady(columns)`, `batchReady(batch)`, `finished(stats {elapsed, rows, affected, notices})` o `failed(Error)`.
6. `ResultTableModel` crea las columnas con `columnsReady` y añade filas con `batchReady`. La vista virtualizada solo crea delegates para las celdas visibles.
7. Si la vista se acerca al final, `fetchMore()` → `SessionWorker::fetchNext(500)`.
8. **Cancelar** (Esc en el editor o Ctrl/Cmd+.): `requestCancel()` se llama directamente desde el hilo de UI y además se marca el token. El provider devuelve `ErrorKind::Cancelled` y la UI muestra "Query cancelled", sin tratarlo como error.
9. Al cerrar la pestaña, se destruye el stream y después la sesión (`QThread::quit()` + `wait()` con timeout; si se pasa, `requestCancel()`).
10. La sentencia, la duración y el resultado (éxito o error) se guardan en el historial (`quarry.db`, Hito de historial).

### 6.1 Contrato de ejecución (H1.6)

Decisiones del 2026-11-14:
- **`SqlSafety` usa un lexer genérico de `core`**, no un tokenizador del dialecto: `ISqlDialect` no tiene `tokenize()` y añadirlo rompería la ABI de los plugins. El lexer reconoce comentarios (`--`, `/* */` anidados), cadenas `'…'` (con `''`), identificadores `"…"`, `` `…` `` y `[…]`, y `$tag$…$tag$`. Basta para SQLite, PostgreSQL y MySQL. `shortcut:` un dialecto con otra sintaxis de comentarios o cadenas puede dar falsos positivos o negativos; si llega uno, añadir un `tokenize()` opcional al dialecto.
- **Ejecutar todo** (Mod+Shift+Enter) ejecuta las sentencias en orden y **se para en el primer error**. Mensajes muestra el resultado de cada una ("Statement 2 of 5: 3 rows affected") y el error, subrayado en el editor.
- **Confirmación única:** antes de ejecutar nada, `SqlSafety` revisa todas las sentencias del plan. Si alguna es peligrosa, un solo diálogo las lista todas, con la conexión y su marca PRODUCTION. Confirmar ejecuta el plan completo; Esc lo cancela antes de empezar.
- **Resultados sin grid:** hasta H1.7, la pestaña Results muestra un resumen ("120 rows · 15 ms") y los nombres de las columnas. Se sigue leyendo el primer lote de 500 filas para probar el streaming y la cancelación.

```cpp
// src/core/include/quarry/sql_safety.h
namespace quarry {
enum class SqlRisk {
    None,
    UpdateWithoutWhere,   // en cualquier conexión
    DeleteWithoutWhere,   // en cualquier conexión
    Drop,                 // solo si production
    Truncate,             // solo si production
};

class SqlSafety {
public:
    // Una sentencia tal como la da splitStatements(). Solo cuenta el comando principal: tras un
    // WITH (CTE) se salta hasta el comando; DELETE/UPDATE necesitan un WHERE a profundidad de
    // paréntesis 0 detrás del comando (un WHERE dentro de un CTE o una subconsulta no vale).
    // Lo que va en comentarios, cadenas e identificadores entrecomillados se ignora.
    // DROP/TRUNCATE cuentan solo como comando principal (no `ALTER TABLE … DROP COLUMN`).
    static SqlRisk check(QStringView statement, bool production);
};
}
```

- `QueryController` (app) tiene una `Session` por pestaña del editor, aparte de la sesión de metadatos del árbol. Se crea en la primera ejecución (con la contraseña del llavero o la recordada en esta ejecución de la app, como el árbol) y se cierra al cerrar la pestaña, al cambiar su conexión o al borrar la conexión.
- Se engancha a `EditorTabsModel::runRequested(plan)`: aplica `SqlSafety` al plan, ejecuta cada rango con `Session::execute` (un `fetchSize` de 500, sin `fetchMore` hasta H1.7) y pasa al siguiente con `finished`. `failed` para el plan; `cancelled` también, y muestra "Query cancelled" sin tratarlo como error.
- Un plan nuevo en la misma pestaña mientras otro se ejecuta cancela el anterior (es lo que ya hace `Session::execute`). Cada pestaña ejecuta lo suyo a la vez que las demás.
- Implementación (H1.6): lo que ve la UI vive en un `QueryState` por pestaña (estado, resumen, `QueryMessagesModel`, error a subrayar), y `QueryController.current` es el de la pestaña actual. Para saber de quién es cada señal final, la pestaña guarda una entrada por `execute()` enviado con el id de su plan; las de un plan reemplazado se descartan, así el `cancelled` del plan viejo no para el nuevo. Si una sentencia acaba antes de que llegue la cancelación, el plan termina igual como "Query cancelled".
- La contraseña se comparte con `ConnectionController` (`readPassword` → `passwordAvailable`/`passwordUnavailable`, `sessionPassword`, `rememberPassword`, `forgetPassword`, que solo borra la recordada en memoria, nunca la del llavero). Un `open()` que falla cierra la sesión; si es de autenticación, se olvida la contraseña recordada para que la siguiente ejecución la pida.
- `QueryController` se declara antes que `ConnectionController` en `MainWindow.qml`: los hijos se destruyen en orden de creación y las sesiones deben morir antes de descargar los plugins.

### 6.2 Contrato del grid (H1.7)

Decisiones del 2026-11-14:
- **Formateo genérico en `core`**, no en el dialecto: `ISqlDialect` no tiene `ValueFormatter` y añadirlo rompería la ABI de los plugins (mismo motivo que el lexer de `SqlSafety`). `CellFormatter` decide por `ValueType`. `shortcut:` si un motor necesita un formato propio (p. ej. intervalos de PostgreSQL), añadir un hook opcional al dialecto.
- **Ejecutar todo con varios resultados:** el grid muestra el resultado de la última sentencia que devolvió columnas; las demás aparecen en Messages con su número de filas.
- **Copiar:** Mod+C copia las celdas seleccionadas en TSV sin cabecera; Mod+Shift+C añade una fila con los nombres de columna. Un NULL se copia como texto vacío. Por encima de 100 000 celdas no se copia nada y se avisa ("Too many cells to copy"; la exportación llega más adelante), para no congelar la UI (decisión del 2026-11-15).

```cpp
// src/core/include/quarry/cell_formatter.h
namespace quarry {
class CellFormatter {
public:
    static constexpr qsizetype kDisplayChars = 500;
    // Texto para pintar una celda. NULL → "NULL"; Bool → "true"/"false"; Int64 en decimal;
    // Double con la representación más corta que vuelve al mismo valor ("0.1", "1e+20", "nan",
    // "inf"); Bytes → "<binary 2.3 KB>" (B, KB, MB con un decimal, base 1024); los tipos de texto
    // (Text, Decimal, Date, Time, DateTime, DateTimeTz, Uuid, Json, Other) tal cual, pero solo
    // hasta el primer salto de línea y kDisplayChars caracteres, con "…" si se corta.
    // En columnas mixtas se usa typeAt(row).
    static QString display(const ColumnBuffer &column, qsizetype row);
    // Texto completo para el portapapeles. NULL → "" ; Bytes → "0x" + hex en minúsculas; el
    // resto como display() pero sin cortar. Tabuladores y saltos de línea se dejan; quien arma
    // el TSV los sustituye por espacios.
    static QString copy(const ColumnBuffer &column, qsizetype row);
    // true si la celda es NULL (el grid la pinta en cursiva y con grid.null).
    static bool isNull(const ColumnBuffer &column, qsizetype row) { return column.isNull(row); }
};
}
```

- `ResultTableModel` (app) guarda los `shared_ptr<const ResultBatch>` tal como llegan (sin copiar) y un índice acumulado de filas por batch; `data(DisplayRole)` busca el batch por bisección y llama a `CellFormatter::display`. `canFetchMore()` es true mientras el último `finished` no traiga `atEnd`; `fetchMore()` pide `Session::fetchMore(500)`. *Fetch all* encadena `fetchMore` y agrupa las inserciones cada ≤ 50 ms.
- `QueryController` conecta `batchReady` y entrega los batches al modelo de la pestaña; un plan nuevo o un cambio de conexión vacía el modelo.
- Implementación (H1.7): `TableView` no llama a `fetchMore`, así que el grid pide la página siguiente (`loadMore()`) cuando `bottomRow` está a ≤ 100 filas del final; las peticiones se encolan con un contador de generación para descartar las de un resultado anterior. Fetch all pide 5 000 filas por petición. El fetch de un plan se registra en `ops` con id 0, así su `finished`/`failed`/`cancelled` nunca avanza ni falla el plan. Un `columnsReady` con columnas sustituye el grid (regla del último resultado); una sentencia sin columnas lo deja como está, marcado "not all loaded" si quedaban filas.
- `ColumnBuffer::shrinkToFit()` se llama en el hilo de sesión cuando un batch está completo, antes de `batchReady` (presupuesto de RAM de H1.7).

### 6.3 Contrato del provider PostgreSQL (H1.8)

Decisiones del 2026-11-15:
- **Árbol:** solo la base de datos del perfil; sus esquemas sin los del sistema (`pg_catalog`, `information_schema`, `pg_toast`, `pg_temp_*`, `pg_toast_temp_*`), y debajo tablas, vistas, vistas materializadas y columnas, leídos de `pg_catalog` (no de `information_schema`, que es más lento y oculta objetos sin privilegios). Otra base de datos = otro perfil. La capacidad `MultipleDatabases` se deja para más adelante.
- **Formulario:** `host` (Text, "localhost"), `port` (Number, 5432), `database` (Text, requerido), `user` (Text, requerido), contraseña (llavero), `sslmode` (Enum: disable, allow, prefer [por defecto], require, verify-ca, verify-full) y `sslrootcert` (Path, opcional, avanzado). Certificados de cliente más adelante. `connect_timeout` = 15 s (§5) y `application_name` = "Quarry".
- **Tests locales:** binarios oficiales en zip de PostgreSQL 17 en `%TEMP%\quarry-pg` (sin instalador, sin admin, sin tocar el PATH), con `initdb` + `pg_ctl` en un puerto libre; se borra al acabar. La conformidad lee `QUARRY_TEST_POSTGRES_URL` y se salta sin ella; en la CI la da el contenedor `postgres:17` del job de Linux.

Cómo cumple la API (§4, §5):
- `execute`: `PQsendQueryParams` (sin parámetros en v1; una sola sentencia, la que da `splitStatements`) + `PQsetChunkedRowsMode(fetchSize)`. Cada `PGRES_TUPLES_CHUNK` llena un `ResultBatch`; `fetch()` lee el siguiente chunk. Sentencias sin filas → `affectedRows` de `PQcmdTuples`. Las notices (`PQsetNoticeReceiver`) van a `ExecutionStats::notices`.
- `requestCancel()`: `PQcancelCreate` + `PQcancelBlocking` desde el hilo que cancela (thread-safe); el objeto de cancelación se crea al abrir. Tras cancelar se drena el resultado (`PQgetResult` hasta null) para que la conexión quede usable.
- Errores: `kind` por SQLSTATE (`PG_DIAG_SQLSTATE`: 28xxx Authentication, 42601 Syntax, 42501 Permission, 23xxx Constraint, 42P01/42703 NotFound, 57014 Cancelled, 08xxx Connection); `message` en claro, `detail` con `PQresultErrorMessage`; `position` = `PG_DIAG_STATEMENT_POSITION` (1-based, en caracteres) convertido a offset 0-based en UTF-16 dentro de la sentencia.
- Tipos por OID: int2/int4/int8 → Int64; float4/float8 → Double; numeric → Decimal (texto exacto); bool → Bool; text/varchar/bpchar/name → Text; bytea → Bytes (decodificado de hex); date/time/timestamp/timestamptz → Date/Time/DateTime/DateTimeTz (texto ISO del servidor, con `DateStyle=ISO`); uuid → Uuid; json/jsonb → Json; arrays y el resto → Other (texto). Resultados en formato texto.
- Dialecto: `quoteIdentifier` con `"…"`, `splitStatements` reconoce `$tag$…$tag$`, `E'…'` y comentarios anidados; `highlightingDefinition` "SQL (PostgreSQL)".
- La DLL de libpq (y OpenSSL) se despliega junto al plugin (§8).

### 6.4 Contrato de edición de datos (H2)

Decisiones del 2026-11-15:
- **Qué se edita:** solo las tablas abiertas desde el árbol (Enter u "Open data" en un nodo tabla abre una pestaña de datos con `SELECT * FROM <tabla>` y paginación). Los resultados de consultas escritas a mano siguen siendo de solo lectura. Las vistas también.
- **Valores como parámetros:** las sentencias generadas llevan placeholders del dialecto y los valores van en `Statement::parameters`. La vista previa muestra exactamente ese SQL y la lista de valores; el test compara lo que recibe el provider con la vista previa.
- **Conflictos:** el `WHERE` usa solo la clave (PK, o ROWID/ctid con aviso). Si un `UPDATE`/`DELETE` no afecta exactamente a 1 fila, se hace rollback de todo, los cambios siguen pendientes y el error nombra la fila.
- **ABI de plugins:** `ISqlDialect` gana tres métodos (todos los plugins se compilan en el árbol y v1 no admite plugins de terceros, §8): `placeholder(int)`, `qualifiedName(ObjectPath)` y `rowIdentifier()`.

```cpp
// src/core/api/include/quarry/provider_api.h, en ISqlDialect
virtual QString placeholder(int index) const = 0;               // 1-based: "?" | "$1"
virtual QString qualifiedName(const ObjectPath &table) const = 0; // "t" | "\"public\".\"t\""
// Columna oculta que identifica una fila sin PK ("rowid" | "ctid"); nullopt = sin ella.
virtual std::optional<QString> rowIdentifier() const = 0;

// src/core/include/quarry/change_set.h
namespace quarry {
struct RowKey {
    enum class Kind : uint8_t { PrimaryKey, RowIdentifier } kind = Kind::PrimaryKey;
    QStringList columns;          // PK, o {rowIdentifier()}
};
struct EditTarget {
    ObjectPath table;
    QStringList columns;          // columnas de la tabla en el orden del grid (sin la oculta)
    RowKey key;
};
struct CellChange { int column = 0; QVariant value; }; // QVariant() = NULL
struct RowEdit {
    enum class Kind : uint8_t { Update, Insert, Delete } kind = Kind::Update;
    QVariantList key;             // valores originales de key.columns (Update/Delete)
    std::vector<CellChange> cells; // Update: celdas cambiadas; Insert: celdas con valor (el resto, DEFAULT)
};
struct PlannedStatement {
    Statement statement;          // sql + parameters, tal cual se ejecutan
    int editIndex = 0;            // la RowEdit de la que sale
    bool expectOneRow = true;     // Update/Delete: debe afectar a 1 fila exacta
};
class ChangeSqlBuilder {
public:
    // Una sentencia por RowEdit, en orden: UPDATE t SET a = $1, b = $2 WHERE id = $3;
    // INSERT INTO t (a, b) VALUES ($1, $2) (o DEFAULT VALUES); DELETE FROM t WHERE id = $1.
    // Identificadores con quoteIdentifier, tabla con qualifiedName. Una clave NULL es un error.
    static Result<std::vector<PlannedStatement>> build(const ISqlDialect &, const EditTarget &,
                                                       const std::vector<RowEdit> &);
};
}

// src/core/include/quarry/session.h
void applyChanges(std::vector<PlannedStatement> statements); // begin → cada una → commit
// signals:
void changesApplied(int statements);
void changesFailed(int statementIndex, quarry::Error error); // tras el rollback; -1 = begin/commit
```

- `Session::applyChanges` corre en el hilo de sesión: `begin()`, cada sentencia (sin filas que leer; `affectedRows` comparado con `expectOneRow`), `commit()`. Cualquier fallo → `rollback()` y `changesFailed`. Cancelar durante la aplicación hace rollback. No se mezcla con un `execute` en curso: si la sesión está ocupada, `changesFailed(-1, …)` sin tocar nada.
- Los valores que escribe el usuario viajan como texto (el servidor convierte según el tipo de la columna); NULL es `QVariant()`. Las celdas BLOB son de solo lectura en H2.
- **Lado de la app (H2b):** `TableEditModel` (`src/app/ui/`) guarda solo los deltas de cada fila, nunca las filas base, sobre el `ResultTableModel` de la pestaña. Tiene historial de deshacer y rehacer, y `buildEdits(&rows)` devuelve las `RowEdit` junto con la fila del modelo de cada una. `QueryController` construye con `ChangeSqlBuilder::build` tanto la vista previa como lo que manda a `applyChanges`, sobre los mismos deltas. Guarda `sentRows[statementIndex]` para llevar un fallo a su fila. Mientras hay un guardado en vuelo (`saving`), el modelo rechaza cualquier cambio.
- **Clave:** con PK se hace `SELECT *`. Sin PK, `SELECT "<rowIdentifier>", * FROM …`, con la primera columna oculta. Una tabla sin PK ni identificador utilizable, o una vista, es de solo lectura.
  - `shortcut:` la PK se lee del detalle "PK" de los nodos de la carpeta Columns (`Session::children`), porque `Session` no tiene `describe()`. Hay que añadir `Session::describe` en core si ese texto deja de ser estable.

### 6.5 Contrato de autocompletado (H3)

Decisiones del 2026-11-15:
- **Metadatos bajo demanda** con `Session::children()`, sin cambiar la API de los providers.
  - Las tablas de cada esquema se cargan la primera vez que se usa el autocompletado en esa conexión.
  - Las columnas de una tabla se cargan solo cuando la sentencia la nombra.
  - La caché es por conexión y usa la sesión de metadatos del árbol (`SchemaTreeModel`).
- **Funciones:** solo las predefinidas del motor, con `ISqlDialect::builtinFunctions()`, una lista estática. Las funciones del usuario llegarán más adelante, junto con el árbol.
- **Apertura:** el popup se abre solo tras `.` y tras 2 o más caracteres de identificador (con 150 ms de espera), y siempre con Ctrl+Space. Un ajuste desactiva la apertura automática.
- **Paleta:** con el prefijo `@` busca tablas y vistas en la misma caché. Al elegir una, se selecciona en el árbol.
- **Ruta de columnas:** la carpeta de columnas de una tabla es `ObjectPath{ table.parts + "Columns", Folder }` en todos los providers, y la conformidad lo comprueba. Así deja de ser un atajo de H2.

```cpp
// src/core/api/include/quarry/provider_api.h, en ISqlDialect
virtual QStringList builtinFunctions() const = 0; // en minúsculas, sin paréntesis: "count", "coalesce"…

// src/core/include/quarry/sql_safety.h, en SqlSafety
// CREATE/ALTER/DROP/RENAME como comando principal (mismas reglas que check()): invalida la caché.
static bool changesSchema(QStringView statement);

// src/core/include/quarry/sql_context.h
namespace quarry {
enum class CompletionScope : uint8_t {
    None,    // dentro de un string, un comentario o un número: no hay sugerencias
    Keyword, // inicio de sentencia o sitio sin ámbito claro: palabras clave, funciones, columnas y tablas
    Table,   // tras FROM, JOIN, UPDATE, INTO, TABLE: tablas, vistas y esquemas
    Column,  // tras SELECT, WHERE, ON, AND, OR, BY, SET, HAVING, ',' en esas cláusulas: columnas, funciones y palabras clave
    Member,  // tras `x.`: columnas de x (alias o tabla); si x es un esquema, sus tablas
};
struct TableReference {
    QStringList parts; // sin comillas, tal como se escribieron: {"users"} | {"public", "users"}
    QString alias;     // vacío si no tiene
};
struct SqlContext {
    CompletionScope scope = CompletionScope::None;
    qsizetype replaceStart = 0; // offset (UTF-16, dentro de la sentencia) donde empieza la palabra que se completa
    QString prefix;             // texto sin comillas de replaceStart al cursor
    QStringList qualifier;      // Member: las partes antes del último '.', sin comillas
    std::vector<TableReference> tables; // todas las de FROM/JOIN/UPDATE/INTO de la sentencia, también tras el cursor
};
// Tokenizador ligero, no un parser: comentarios, strings, identificadores "x", `x`, [x], paréntesis.
// Cursor en [0, statement.size()]. Puro, sin E/S; O(n) en la longitud de la sentencia.
SqlContext analyzeSqlContext(QStringView statement, qsizetype cursor);
}

// src/core/include/quarry/metadata_cache.h
namespace quarry {
struct CachedObject {
    ObjectPath path;
    QString name;
    QString detail; // el de SchemaNode: tipo de columna, "PK"…
};
class MetadataCache : public QObject { // hilo de UI; las respuestas de Session llegan encoladas
    Q_OBJECT
public:
    enum class State : uint8_t { Empty, Loading, Ready, Failed };
    explicit MetadataCache(QObject *parent = nullptr);
    void attach(Session *session); // no se adueña; nullptr desconecta e ignora las respuestas tardías
    // Recorre desde la raíz los nodos Database/Schema/Folder y se detiene en Table/View/
    // MaterializedView, sin cargar sus columnas. Idempotente mientras carga o ya cargó.
    void load();
    void ensureColumns(const ObjectPath &table); // una sola vez por tabla; emite changed() al llegar
    void invalidate();                           // lo vacía todo; el siguiente load() empieza de cero
    State state() const;
    // Consultas sin E/S. resolve() busca por nombre sin distinguir mayúsculas, con el esquema si se
    // da; si hay varias, gana la primera en el orden del árbol.
    std::optional<ObjectPath> resolve(const QStringList &parts) const;
    const std::vector<CachedObject> &relations() const;               // tablas, vistas, vistas materializadas
    const std::vector<CachedObject> &schemas() const;
    const std::vector<CachedObject> *columns(const ObjectPath &table) const; // nullptr: no cargadas
signals:
    void changed();
    void failed(quarry::Error error);
};
}

// src/core/include/quarry/completion.h
namespace quarry {
enum class SuggestionKind : uint8_t { Keyword, Function, Schema, Table, View, Column };
struct Suggestion {
    QString text;   // lo que se inserta en [replaceStart, cursor), ya entrecomillado si hace falta
    QString label;  // lo que se muestra (sin comillas)
    SuggestionKind kind;
    QString detail; // tipo de columna, nombre de la tabla…
};
class CompletionEngine {
public:
    // Sin distinguir mayúsculas: primero los que empiezan por el prefijo, luego los que lo
    // contienen; dentro, por tipo según el ámbito y por orden alfabético. Como mucho `limit`.
    static std::vector<Suggestion> complete(const SqlContext &, const MetadataCache &,
                                            const ISqlDialect &, int limit = 200);
    // Tablas del contexto cuyas columnas aún no están en la caché: la app llama a ensureColumns().
    static std::vector<ObjectPath> missingColumns(const SqlContext &, const MetadataCache &);
};
}
```

- Los identificadores se entrecomillan con `quoteIdentifier` cuando no son `[A-Za-z_][A-Za-z0-9_]*` o coinciden con una palabra clave. Las palabras clave se insertan en mayúsculas.
- **Presupuesto:** `complete()` tarda ≤ 50 ms con 10 000 tablas en caché (`bench_completion`; el objetivo real son unos pocos ms). Todo corre en el hilo de UI sin E/S: la carga va por la `Session` y la UI se actualiza con `changed()`.
- **Ajustes de implementación (H3a):**
  - **Índices de búsqueda:** `MetadataCache` también expone `relationKeys()` (nombres en minúsculas, paralelos a `relations()`) y `relationOrder()` (índices ordenados por nombre), para buscar por prefijo con búsqueda binaria.
  - **Esquemas:** en SQLite no hay esquemas, así que `schemas()` lista los nodos Database ("main"…). Pasa lo mismo con cualquier Database cuyos hijos no sean Schema.
  - **Fallos:** si falla la carga de las columnas, se emite `failed()` pero el estado no cambia, y otro `ensureColumns()` lo reintenta. Solo un fallo del recorrido pone `State::Failed`.
  - **`Suggestion::detail`:** en el ámbito Member, una columna muestra su tipo; en los ámbitos Column y Keyword, "tabla: tipo". Una tabla o vista muestra su esquema.
  - **`missingColumns()`:** también devuelve la tabla del calificador de Member, aunque no esté en el FROM.
  - **Contexto:** la palabra que se escribe nunca cuenta como tabla ni como alias. ON siempre abre una cláusula de columnas.
  - **Límites:** sin ámbito por subconsulta (una sola lista de tablas). `[` es un identificador entrecomillado salvo pegado a una palabra (`a[1]`). Si ya hay una comilla de cierre tras el cursor, la app debe tenerla en cuenta al reemplazar.
  - **Bench:** `bench_completion`, con 11 429 relaciones × 20 columnas: p95 ≤ 0.58 ms y máximo 2.8 ms en todos los escenarios.
- **Lado de la app:** `CompletionController` tiene una `MetadataCache` por conexión y la engancha a la sesión de metadatos del árbol. Abre esa sesión si hace falta, pero nunca pide la contraseña mientras se escribe: sin sesión, solo hay palabras clave y funciones, y se muestra "Connect to suggest tables". La caché se invalida con el comando "Refresh metadata", al refrescar el árbol y tras ejecutar con éxito una sentencia en la que `SqlSafety::changesSchema()` es verdadero.

### 6.6 Contrato del provider MySQL / MariaDB (H6)

Decisiones del 2026-11-17 (ver ROADMAP H6). Plugin `mysql` en `src/providers/mysql/`, con `libmariadb` (MariaDB Connector/C 3.4, vcpkg). Habla con MySQL 8.x y con MariaDB 10.x/11.x. No cambia ningún header de `core`.
- **Árbol:** todas las bases de datos visibles sin las del sistema (`mysql`, `sys`, `information_schema`, `performance_schema`), y debajo tablas, vistas y columnas, más índices y claves foráneas si la API de PostgreSQL ya los muestra; leídos de `information_schema`. `database` es opcional (base por defecto de la sesión). Capacidad `multipleDatabases`.
- **Formulario:** `host` (Text, "localhost"), `port` (Number, 3306), `database` (Text, opcional), `user` (Text, requerido), contraseña (llavero), `sslmode` (Enum: disable, prefer [por defecto], require, verify-ca, verify-full) y `sslrootcert` (Path, opcional, avanzado). `connect_timeout` = 15 s, `charset` utf8mb4 (`SET NAMES utf8mb4` al abrir) y `program_name` = "Quarry". **prefer** se emula: se intenta con TLS (`MYSQL_OPT_SSL_ENFORCE`) y, si el servidor no lo ofrece o falla el apretón, se reintenta sin TLS; `require` y superiores no vuelven atrás; `verify-ca` y `verify-full` usan `MYSQL_OPT_SSL_VERIFY_SERVER_CERT` y `sslrootcert`.
- **Servidor:** al abrir se lee la versión (`mysql_get_server_info`); si contiene "MariaDB" el `ServerInfo` lo indica y el dialecto usa las funciones y palabras clave de MariaDB; si no, las de MySQL.
- **Ejecución:** `mysql_real_query` (texto, una sola sentencia, la que da `splitStatements`; sin parámetros en v1) + `mysql_use_result` + `mysql_fetch_row`/`mysql_fetch_lengths` en lotes de `fetchSize`. Sentencias sin filas: `affectedRows` de `mysql_affected_rows`. Las notices (`SHOW WARNINGS` tras un resultado con `mysql_warning_count()` > 0) van a `ExecutionStats::notices`. Un resultado sin consumir deja la conexión ocupada: al destruir un stream sin terminar se hace `KILL QUERY` por la conexión auxiliar antes de `mysql_free_result`, para que no lea el resto del resultado.
- **Cancelación:** `requestCancel()` (thread-safe) abre, si hace falta y con las mismas credenciales, una conexión auxiliar de vida corta (timeout 5 s) y ejecuta `KILL QUERY <mysql_thread_id>`. Tras cancelar se drena o libera el resultado para que la conexión quede usable. El error 1317 (`ER_QUERY_INTERRUPTED`) se traduce a `ErrorKind::Cancelled`.
- **Errores:** `kind` por `mysql_errno`: 1045 y 1698 Authentication; 1044, 1142, 1143 y 1227 Permission; 1062, 1048, 1216, 1217, 1451, 1452 y 3819 Constraint; 1064 y 1149 Syntax; 1049, 1054, 1146 y 1305 NotFound; 1317 Cancelled; 1205 y 1969 Timeout; 2002, 2003, 2005, 2006 y 2013 Connection; el resto Driver. `message` en claro y `detail` con `mysql_error()` y el SQLSTATE; MySQL no da posición, así que `position` queda vacía.
- **Tipos** por `MYSQL_TYPE_*` y el juego de caracteres de la columna: TINY, SHORT, INT24, LONG, LONGLONG y YEAR → Int64 (un `tinyint(1)` no es Bool); FLOAT y DOUBLE → Double; DECIMAL y NEWDECIMAL → Decimal (texto exacto); DATE → Date; TIME → Time; DATETIME y TIMESTAMP → DateTime (texto del servidor, sin zona); JSON → Json (en MariaDB es LONGTEXT con alias, se trata como Text); BLOB, VARBINARY, BINARY y los de charset 63 → Bytes; VARCHAR, STRING, TEXT, ENUM y SET con otro charset → Text; BIT → Int64 si cabe en 64 bits; GEOMETRY y el resto → Other (texto).
- **Dialecto:** `quoteIdentifier` con `` `…` `` (los duplica); `quoteLiteral` con `'…'`, duplicando la comilla y doblando la barra invertida; `highlightingDefinition` "SQL (MySQL)"; `splitStatements` reconoce `DELIMITER`, comentarios `#`, `-- ` (con espacio) y `/* */` (sin anidar, con `/*! … */` como código), cadenas `'…'` y `"…"` con `\` y `''`, e identificadores con comillas invertidas.
- **Despliegue:** `libmariadb` (y OpenSSL en Linux/macOS) junto a `quarry.exe` (§8). Los plugins de autenticación de Connector/C (`caching_sha2_password`, `mysql_clear_password`, `sha256_password`, `client_ed25519`, `dialog`) son librerías dinámicas que se cargan desde `MYSQL_PLUGIN_DIR`: se copian a `<carpeta de quarry.exe>/mariadb-plugins/` (nunca a `plugins/quarry`, que rechaza cualquier librería que no sea un plugin de Quarry) y el provider fija esa ruta con `mysql_options(MYSQL_PLUGIN_DIR)` al crear la conexión. Hace falta `caching_sha2_password` para MySQL 8.4.
- **Tests locales:** zip portable de MariaDB 11.8 en `%TEMP%\quarry-maria` (servidor en `127.0.0.1:53306`, usuario `quarry`, base `quarry_test`, contraseñas fuera del repositorio); se para y se borra al acabar el hito. La conformidad lee `QUARRY_TEST_MYSQL_URL` y se salta sin ella; en la CI la dan los contenedores `mariadb:11` y `mysql:8.4` del job de Linux.
- **Detalles fijados en la implementación:**
  - Parámetros: aunque §6.6 los deja fuera de v1, el contrato de conformidad y `applyChanges` necesitan enlazar `?`. Se enlazan en el cliente con `mysql_real_escape_string` (respeta `NO_BACKSLASH_ESCAPES`), saltando cadenas, comentarios e identificadores.
  - Errores añadidos: 4025 (CHECK de MariaDB) → Constraint y 2026 (fallo de TLS) → Connection; cualquier otro error sin mapear al conectar es Connection. Un plugin de autenticación que no se puede cargar se traduce a Authentication, con la causa real en `detail`.
  - Banderas: `CLIENT_FOUND_ROWS` (un `UPDATE` informa de las filas que coinciden, no de las que cambian; si no, el editor de datos vería un `UPDATE` sin cambios como una actualización perdida) y TCP forzado, también para "localhost".
  - Dialecto: `dialect()` no conoce el servidor, así que palabras clave y funciones son la unión de MariaDB y MySQL; `ServerInfo.product` dice "MariaDB" o "MySQL". `DELIMITER` solo se reconoce al principio de una línea, como en el cliente `mysql`. `explainPrefix(Analyze)` da `EXPLAIN ANALYZE`, que MariaDB rechaza (usa `ANALYZE <sentencia>`): marcado con `// shortcut` para H9.
  - Conformidad: `Fixture::hasRowIdentifier()` y `Fixture::reportsErrorPosition()` (por defecto, el comportamiento de SQLite y PostgreSQL). `UPDATE … SET name = name || '!'` pasó a `SET id = id + 10`, porque `||` es OR en MySQL y MariaDB.
- **Plugins de autenticación:** comprobado con MariaDB 11.8 que ninguno de los cinco está dentro de libmariadb: sin `MYSQL_PLUGIN_DIR` correcto no carga ninguno, y con `<carpeta de quarry.exe>/mariadb-plugins` cargan todos (`caching_sha2_password` probado con TLS y sin él). Queda para la CI el servidor MySQL 8.4, un servidor sin TLS y Linux/macOS (RPATH `$ORIGIN`/`@loader_path`, sin verificar).
- **Límites conocidos:** de un `CALL` solo se lee el primer resultado (el resto se drena y `nextResult` devuelve false). Una cancelación que llega justo cuando la sentencia termina por sí sola se informa como Cancelled aunque se haya confirmado. `verify-ca` también comprueba el nombre del host (libmariadb solo tiene una bandera de verificación). `tinyint(1)` es Int64, no Bool. Con contraseña vacía libmariadb recurre a la variable de entorno `MYSQL_PWD`. No se ha confirmado en el código de libmariadb 3.x que `mysql_thread_init`/`mysql_thread_end` sean innecesarios: se llaman en `open()` y en el hilo de cancelación.

### 6.7 Contrato de los providers ODBC y SQL Server (H7)

Decisiones del 2026-11-17 (ver ROADMAP H7). Dos plugins, `odbc` ("ODBC genérico") y `mssql` ("SQL Server"), sobre una librería estática común `quarry_odbc_common` (`src/providers/odbc_common/`: entorno y conexión ODBC, stream, tipos y errores). No cambia ningún header de `core`. Un plugin no depende del otro. Capacidades: `odbc` → `cancel`, `transactions`, `multipleResultSets`; `mssql` → además `schemas` y `multipleDatabases`.
- **ODBC 3.8** (`SQL_ATTR_ODBC_VERSION`), Unicode: `SQLDriverConnectW`, `SQLExecDirectW`/`SQLPrepareW` y `SQL_C_WCHAR` para el texto (UTF-16 en el driver, UTF-8 en Quarry). Windows enlaza `odbc32` del sistema; Linux y macOS, unixODBC de vcpkg (dinámica: los drivers cargan su `libodbcinst`, así que solo puede haber una copia). `find_package(ODBC)` con el target `ODBC::ODBC` en los dos plugins. `libodbc` y `libodbcinst` van junto a los ejecutables (§8). El Driver Manager busca `odbcinst.ini` en su prefijo de vcpkg: para drivers registrados por nombre en `/etc` hay que definir `ODBCSYSINI=/etc`; con `DRIVER={ruta}` no hace falta.
- **Ejecución:** `SQLExecDirectW` (sin parámetros) o `SQLPrepareW` + `SQLBindParameter` con `?` (`placeholder()` devuelve `?`). Una sola sentencia por `execute` (la que da `splitStatements`). Resultados con `SQLNumResultCols` + `SQLDescribeColW` y `SQLFetchScroll`/`SQLFetch` con *row-wise binding* de bloques de `fetchSize` filas para tipos de tamaño acotado; texto y binarios largos o sin tamaño (`SQL_NO_TOTAL`, `max`, LOB) con `SQLGetData` por trozos. `affectedRows` por `SQLRowCount`. Varios resultados: `SQLMoreResults`. Los mensajes informativos (`SQL_SUCCESS_WITH_INFO`, `SQLGetDiagRecW`) van a `ExecutionStats::notices`. Un solo cursor activo por conexión (MARS desactivado): el árbol usa su propia sesión.
- **Cancelación:** `requestCancel()` llama a `SQLCancel` sobre el *statement* en curso desde el hilo que cancela (thread-safe por especificación ODBC 3.x). El SQLSTATE `HY008` y el error nativo 3617 se traducen a `ErrorKind::Cancelled`. Tras cancelar se cierra el cursor (`SQLFreeStmt(SQL_CLOSE)`) para que la conexión quede usable. Un resultado a medias se cierra con `SQLCancel` antes de `SQLFreeStmt`, para no leer el resto.
- **Errores:** por SQLSTATE de `SQLGetDiagRecW`: `28000` y `28xxx` Authentication; `08xxx` e `IM002`/`IM003`/`IM004`/`IM010`/`IM014` Connection (driver o DSN no encontrado: mensaje claro con el nombre pedido); `HYT00`/`HYT01` Timeout; `HY008` Cancelled; `23xxx` Constraint; `42S02`/`42S12`/`42S22` NotFound; `42000` Syntax salvo que el código nativo de SQL Server sea 229, 230, 297, 262 o 300 (Permission), 207 o 208 (NotFound) o 2627, 2601, 515 o 547 (Constraint); el resto Driver. `message` en claro y `detail` con SQLSTATE, código nativo y texto del driver. SQL Server no da posición: `position` queda vacía.
- **Tipos** por `SQL_*` de `SQLDescribeCol` (y el tipo nativo del driver para los extendidos): `SQL_TINYINT`, `SQL_SMALLINT`, `SQL_INTEGER` y `SQL_BIGINT` → Int64; `SQL_REAL`, `SQL_FLOAT` y `SQL_DOUBLE` → Double; `SQL_DECIMAL` y `SQL_NUMERIC` → Decimal (texto exacto); `SQL_BIT` → Bool; `SQL_CHAR`, `SQL_VARCHAR`, `SQL_LONGVARCHAR` y sus variantes `W` → Text; `SQL_BINARY`, `SQL_VARBINARY` y `SQL_LONGVARBINARY` → Bytes; `SQL_TYPE_DATE` → Date; `SQL_TYPE_TIME` y `SQL_SS_TIME2` (-154) → Time; `SQL_TYPE_TIMESTAMP` → DateTime; `SQL_SS_TIMESTAMPOFFSET` (-155) → DateTimeTz; `SQL_GUID` → Uuid; `SQL_SS_XML` (-152) → Text; `SQL_SS_UDT` y el resto → Other (texto o hex). Fechas y horas como texto ISO.
- **`odbc` (genérico):** formulario `dsn` (Text), `driver` (Text: nombre registrado o ruta de la librería), `connectionString` (Text, avanzado: `clave=valor;…` sin `PWD`), `user` (Text) y contraseña (llavero; se añade como `PWD={…}` escapando `}`). Hace falta al menos `dsn`, `driver` o `connectionString`. Árbol y metadatos con `SQLTables`, `SQLColumns`, `SQLPrimaryKeys`, `SQLForeignKeys` y `SQLStatistics`: catálogo → esquema → tabla, sin las tablas de sistema (`SYSTEM TABLE`). Dialecto ANSI: comillas dobles y `OFFSET … FETCH`. El dialecto es estático y no conoce el driver (`// shortcut`: leer `SQL_IDENTIFIER_QUOTE_CHAR` y las palabras reservadas del driver cuando haya conexión). Sin `explain`.
- **`mssql` (SQL Server):** formulario `server` (Text, "localhost"; admite `host` y `host\instancia`), `port` (Number, 1433; se ignora si hay instancia), `database` (Text, opcional), `user` (Text; requerido con autenticación SQL), contraseña (llavero), `authentication` (Enum: sql [por defecto], windows), `encrypt` (Enum: mandatory [por defecto], optional, strict), `trustservercertificate` (Bool, falso, avanzado) y `driver` (Text, avanzado, opcional). Con `windows` se usa `Trusted_Connection=yes` (Kerberos en Linux y macOS: sin probar). Driver: el indicado o, si no, el primero de "ODBC Driver 18 for SQL Server" y "ODBC Driver 17 for SQL Server" que devuelva `SQLDrivers`; si no hay ninguno, error `Connection` con el texto "Install the Microsoft ODBC Driver 18 for SQL Server." `Application Name=Quarry`, `LoginTimeout=15`. `strict` solo con Driver 18.
- **Árbol `mssql`:** bases de datos visibles y en línea sin las del sistema (`master`, `tempdb`, `model`, `msdb`), de `sys.databases`; debajo, esquemas sin los del sistema (`sys`, `INFORMATION_SCHEMA`, `guest` y los `db_*`) y, en cada uno, tablas, vistas y columnas, más índices y claves foráneas, de `[base].sys.*` (`sys.schemas`, `sys.tables`, `sys.views`, `sys.columns`, `sys.indexes`, `sys.foreign_keys`). Capacidades `schemas` y `multipleDatabases`. Los nombres se citan con `[…]` y se arma `[base].[esquema].[tabla]`.
- **Dialecto T-SQL:** `quoteIdentifier` con `[…]` (duplica `]`); `quoteLiteral` con `'…'` duplicando la comilla y prefijo `N` para Unicode; `highlightingDefinition` "SQL (Transact-SQL)"; `limitClause` → `OFFSET n ROWS FETCH NEXT m ROWS ONLY` (exige `ORDER BY`: la app antepone `ORDER BY (SELECT NULL)` si falta; se documenta); `explainPrefix` vacío (H9); `rowIdentifier()` vacío; `splitStatements`: las líneas que solo contienen `GO` (con recuento opcional, que se ignora) separan lotes; `;` separa sentencias salvo dentro de bloques `BEGIN … END` y `CASE … END` (sin contar `BEGIN TRAN[SACTION]`, `BEGIN DISTRIBUTED`, `BEGIN CONVERSATION` ni `BEGIN DIALOG`; `BEGIN TRY` y `BEGIN CATCH` sí abren bloque); comentarios `--` y `/* */` anidados; cadenas `'…'`, `N'…'` y `"…"`; identificadores `[…]`. Palabras clave y funciones propias de T-SQL (`TOP`, `ISNULL`, `GETDATE`, `NEWID`…).
- **Despliegue:** Windows no necesita nada (`odbc32`). Linux y macOS: `libodbc` y `libodbcinst` de vcpkg junto a los ejecutables (RPATH `$ORIGIN` / `@loader_path`). Los drivers (Microsoft ODBC Driver 17/18, psqlodbc…) los instala el usuario.
- **Tests:** el Administrador de controladores de Windows solo carga drivers registrados en HKLM (hace falta administrador): `DRIVER={ruta}` no funciona allí. En local (Windows) corren los tests sin servidor (dialecto, `splitStatements`, mapas de errores y tipos, construcción de la cadena de conexión) y una prueba de humo con el driver "Microsoft Access Text Driver (*.txt, *.csv)", ya instalado, sobre CSV en `%TEMP%` (conectar, `SELECT`, `SQLTables`, `SQLColumns`, tipos); se salta si el driver no está. La conformidad completa corre solo en la CI, en el job de Linux: `odbc` con `odbc-postgresql` (driver "PostgreSQL Unicode") contra el contenedor `postgres:17`, y `mssql` con `msodbcsql18` contra `mcr.microsoft.com/mssql/server:2022-latest`. La conformidad lee `QUARRY_TEST_ODBC_URL` (una cadena de conexión ODBC completa) y `QUARRY_TEST_MSSQL_URL` (`mssql://usuario:clave@host:puerto/base?trustservercertificate=true`) y se salta sin ellas. La de `odbc` supone un servidor compatible con PostgreSQL (aísla cada test con un esquema).
- **Detalles fijados en la implementación:**
  - Enteros que caben en 32 bits se enlazan como `SQL_C_SLONG` (el driver de texto de Microsoft rechaza `SQL_C_SBIGINT`). Los parámetros NULL toman el tipo de `SQLDescribeParam`.
  - Resultados: enlace por bloques solo para el prefijo de columnas de tamaño acotado; desde la primera columna larga todo va por `SQLGetData`, fila a fila. `odbc` no enlaza texto, binario ni decimal a ciegas (psqlodbc da tamaños poco fiables); `mssql` sí. Un valor más largo que el tamaño enlazado se recorta y se avisa ("Some values were cut short."). Los resultados que solo traen recuento se saltan hasta el primer rowset y los recuentos se suman en `affectedRows`.
  - `requestCancel()` repite `SQLCancel` cada 50 ms desde un hilo auxiliar mientras dure la misma llamada, para cubrir la ventana en que el driver aún no ha entrado en `SQLExecDirect`. Un cancel que llega justo al terminar se informa como Cancelled.
  - Errores añadidos por psqlodbc: `42P01` y `42703` NotFound, `42501` Permission, `42601` Syntax y `57014` Cancelled; el código nativo 4060 (base inexistente al conectar) es NotFound.
  - Árbol genérico: las rutas de tabla son siempre `[catálogo, esquema, tabla]` (partes ausentes como ""). Transacciones con `SQL_ATTR_AUTOCOMMIT` y `SQLEndTran`.
  - Conformidad: único gancho nuevo `Fixture::secrets()` (por defecto vacío); se reutilizan `hasRowIdentifier`, `reportsErrorPosition` y `fractionalLiteralType` de MySQL. `mssql` crea `quarry_conf_<pid>_<n>` por test; `odbc` crea un esquema `quarry_conf_<pid>_<n>` con `search_path` y `UseServerSidePrepare=0`.
- **Límites conocidos:** `splitStatements` de T-SQL parte `IF x …; ELSE …` sin `BEGIN` en el `;` anterior a `ELSE` (`// shortcut`). Las cadenas de más de 4000 caracteres se enlazan como `SQL_WLONGVARCHAR` (`ntext`) y los binarios de más de 8000 bytes como `SQL_LONGVARBINARY` (`image`), que no se comparan con `=`. El dialecto genérico es estático (sin `SQL_IDENTIFIER_QUOTE_CHAR` ni palabras reservadas del driver). Sin posición de error ni `explain`. `nextResult` puede devolver un resultado vacío cuando solo queda un mensaje informativo al final.
- **Sin verificar en local:** Linux y macOS (compilación, RPATH, `libodbc`/`libodbcinst`, `ODBCSYSINI`) y todo lo que necesita un servidor: conformidad, cancelación sobre `WAITFOR`/`pg_sleep`, `SQLGetData` de `NVARCHAR(MAX)` y `VARBINARY(MAX)`, `PRINT`/`RAISERROR` a notices, varios resultados, transacciones y las consultas `sys.*`. Si la CI falla, mirar primero: los parámetros de `applyChanges` contra PostgreSQL, si psqlodbc informa catálogo y `SQLCancel` con psqlodbc.

### 6.8 Contrato de los túneles SSH (H8)

- **Alcance:** PostgreSQL, MySQL/MariaDB y SQL Server (`Capability::Ssh`). ODBC genérico queda fuera (el host va dentro de la cadena del driver) y SQLite no muestra la sección. Sin `ProxyJump` ni varios saltos.
- **Configuración (sin secretos)** en `profile.values` con prefijo `ssh.`: `ssh.enabled`, `ssh.host`, `ssh.port` (22), `ssh.user`, `ssh.auth` (`password` | `key` | `agent`), `ssh.keyPath`. Tipo `SshTunnelConfig` en `src/core/include/quarry/`.
- **Secretos** en el llavero, junto a la contraseña de la base: `connection/<uuid>/ssh-password` y `connection/<uuid>/ssh-passphrase`. `Secrets` gana `sshPassword` y `sshPassphrase`. Mismo modelo "guardar / pedir siempre" y sin alternativa a disco.
- **`SshTunnel`** (core, libssh2 aislado en `src/core/src/ssh/`): escucha en `127.0.0.1:0`, reenvía cada conexión entrante por `direct-tcpip` y trabaja en un hilo propio. `open()` es bloqueante y atiende un `CancellationToken`; se llama desde el hilo de la sesión, nunca desde la UI. Devuelve `Result<>` con `ErrorKind::Connection` o `Auth` y el texto de libssh2 en `detail`.
- **Integración:** `ProviderRegistry::createConnection` devuelve un decorador de `IConnection` si `ssh.enabled`. En `open()` verifica la huella, abre el túnel, crea la conexión real con `host`/`port` reescritos a `127.0.0.1:<puerto local>` y la abre; `close()` cierra el túnel. `app` y los providers no cambian.
- **Huellas:** archivo propio `known_hosts` en la carpeta de datos de Quarry (no se toca `~/.ssh`), gestionado con `libssh2_knownhost_*`. Resultados `Match`, `Unknown` y `Mismatch`. `Unknown` pide confirmar la huella SHA256 (acepta una vez o la guarda); `Mismatch` bloquea la conexión con un error claro y no se puede aceptar desde el diálogo de conexión.
- **API pública (headers en `src/core/include/quarry/`):** `ssh_tunnel_config.h` (`SshTunnelConfig`, `SshAuth`), `ssh_host_key.h` (`HostKeyInfo`, `HostKeyStatus`, `KnownHostsStore`, `SshHostKeyChecker`). `SshHostKeyChecker` (hilo propio) conecta, lee la clave del servidor en el handshake y se desconecta antes de autenticar: `check()` → `checked(HostKeyInfo)`; `accept(info, remember)` guarda la huella o la confía solo para esta ejecución. Un `KnownHostsStore` compartido (`std::shared_ptr`, inyectado con `ProviderRegistry::setKnownHosts`) lo usan el comprobador y los túneles.
- **Flujo en la app:** antes de crear la conexión, si `ssh.enabled`, se comprueba la huella; `Unknown` abre el diálogo de confirmación, `Mismatch` muestra el error y no se conecta. Si pese a ello el túnel encuentra una clave no fiable (carrera, `known_hosts` editado), `open()` falla con `Error::sqlState` = `kSshUntrustedState` o `kSshMismatchState` y la huella en `detail`.
- **Secretos en `CredentialStore`:** `SecretKind` (`Password`, `SshPassword`, `SshPassphrase`) y operaciones `readSecret`/`writeSecret`/`deleteSecret` y `rememberForSession` por tipo; las de contraseña de la base siguen funcionando. Borrar una conexión borra los tres.
- **Autenticación:** contraseña, clave privada (con frase de paso opcional) y agente (pageant y OpenSSH agent en Windows, `SSH_AUTH_SOCK` en Linux y macOS).
- **Pruebas:** tests unitarios locales (configuración, `known_hosts`, errores) y, en el job de Linux de la CI, el `sshd` del propio runner (puerto 2222, contraseña, reenvío TCP) y los tests levantan un servidor de eco local al que llegan por el túnel (`QUARRY_TEST_SSH_URL`; `QUARRY_TEST_SSH_TARGET` opcional si el `sshd` está en otra máquina). En Windows y macOS los tests de servidor se saltan.
- **Detalles fijados en la implementación:**
  - `known_hosts` se lee y escribe con código propio (libssh2 informa como "no encontrado" una clave de otro tipo). Coincidencia exacta por host (sin comodines ni nombres con hash); `[host]:puerto` si el puerto no es 22. Misma clave = `Match`; otra clave del mismo tipo = `Mismatch`; solo hay claves de otro tipo = `Unknown`.
  - El handshake pide primero los tipos de clave que ya constan para ese host (`KnownHostsStore::knownKeyTypes`, vía `libssh2_session_method_pref`), para que un servidor conocido presente la clave guardada. Un servidor que solo ofrezca otro tipo sale como `Unknown` y pide confirmar la huella, como hace OpenSSH con su aviso.
  - Los fallos de login en el servidor SSH llevan `Error::sqlState` = `kSshAuthState` para distinguirlos de los de la base.
  - El modo del secreto SSH (llavero o pedir siempre) va en `profile.values["ssh.secretMode"]` (`keychain` | `ask`). `ConnectionPreflight` (app, propiedad de `ConnectionController`) resuelve el secreto y la huella antes de crear la conexión en consultas, árbol, exportación y el botón Test; exportación y autocompletado no abren diálogos y fallan con un mensaje que indica dónde confirmar la huella o el secreto.
  - El destino del túnel es `values["host"]` (`values["server"]` en `mssql`) y `port` (por defecto el del provider o 5432/3306/1433).
- **Límites conocidos:** `mssql` con `host\instancia` túnela al puerto del perfil o 1433 (la consulta a SQL Browser no pasa por el túnel); con `encrypt=mandatory` contra `127.0.0.1` el nombre del certificado puede no coincidir; la resolución DNS no se puede interrumpir (una cancelación se nota al volver); el backend `wincng` de Windows puede rechazar claves privadas ed25519 en formato OpenSSH; los textos de error de core no están en `quarry_es.ts`.
- **Sin verificar en local:** servidor SSH real (contraseña, clave, agente, reenvío, varios clientes, cierre con transferencia), Linux y macOS, lectores de pantalla y el selector nativo de archivos para la clave. Mirar primero si la CI falla: backend criptográfico de libssh2, agente, y el cierre del túnel con una consulta en curso.

### 6.9 Contrato de EXPLAIN visual (H9)

- **Alcance:** SQLite, PostgreSQL y MySQL/MariaDB (`Capability::Explain`). SQL Server y ODBC quedan fuera.
- **Tipos** en `quarry_provider_api` (`explain_plan.h`): `ExplainNode` (etiqueta, detalles clave-valor, coste de inicio y total, filas estimadas y reales, tiempo real, avisos, hijos) y `ExplainPlan` (raíz, salida original como texto, motor, `truncated`). Los números ausentes son `std::optional`.
- **`ISqlDialect`:** `explainStatement(sql, ExplainMode, product)` da la sentencia que se ejecuta (`EXPLAIN (FORMAT JSON)` / `EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON)` en PostgreSQL, `EXPLAIN FORMAT=JSON` / `EXPLAIN ANALYZE` (formato árbol, el único que admite MySQL 8.4) en MySQL, `ANALYZE FORMAT=JSON` en MariaDB, `EXPLAIN QUERY PLAN` en SQLite) y `parseExplain(output, ExplainMode, product)` devuelve `Result<ExplainPlan>`; `output` son las celdas de cada fila unidas por tabulador y las filas por salto de línea (los motores JSON devuelven una sola celda). Ambos métodos tienen una implementación por defecto (vacío / `Unsupported`). `product` es `ServerInfo::product`. Los costes están en unidades del motor y no se comparan entre motores; un número ausente es `nullopt`, no 0. Cuando el motor da varios pasos de nivel superior (SQLite), el parser los cuelga de una raíz sin etiqueta que la UI oculta. Cada provider conoce su formato; MySQL y MariaDB tienen un parser cada uno, elegido por `ServerInfo.product`. `explainPrefix` se sustituye por `explainStatement` (el atajo de MariaDB de H6 desaparece). Detalles fijados en la implementación: `nodeCount` y el tope de 5000 no cuentan la raíz oculta; `actualTimeMs` en PostgreSQL es `Actual Total Time` × `Actual Loops` y `actualRows` queda por bucle; los parsers limitan la profundidad a 200 niveles (error `Syntax`); en MySQL, `estimatedRows` sale de `rows_produced_per_join` (o `rows_examined_per_scan`); el aviso "Row estimate off by Nx" también existe en MariaDB y en el formato árbol.
- **Ejecución:** la app lanza la sentencia de `explainStatement` por la `Session` de la pestaña, como cualquier consulta. El parseo corre en el hilo de UI hasta 256 KiB de texto y, por encima, en un `QThreadPool` de un hilo propiedad de `QueryController` (se une al destruirlo); un plan de más de 32 MiB no se conserva y se muestra un error. Tope de 5000 nodos: el resto se descarta y `truncated` se anuncia. Un error de parseo muestra la salida original como texto y un aviso, nunca una pestaña vacía.
- **`ANALYZE`:** solo para sentencias de lectura sin efectos (`SELECT`, `WITH … SELECT`; la app lo decide con `isReadOnlyQuery` en `query_controller.cpp`, porque `SqlSafety` no tiene esa pregunta: rechaza INSERT/UPDATE/DELETE/MERGE, también dentro de una CTE, e `INTO`; una función con efectos dentro de un `SELECT` cuenta como lectura y la cubre la confirmación) y siempre con confirmación, porque la consulta se ejecuta entera. Para `INSERT`/`UPDATE`/`DELETE`/DDL solo se ofrece el plan sin ejecutar.
- **UI:** pestaña "Plan" en el panel de resultados, junto a Resultados y Mensajes. Árbol accesible con teclado y lector de pantalla (coste, filas y porcentaje de coste por nodo), barra proporcional **y** porcentaje en texto (nunca solo color), panel de detalle del nodo seleccionado y pestaña de texto con la salida original. Acciones "Explain" y "Explain analyze" en el editor, atajo y paleta de comandos. Sin diagrama gráfico de cajas y flechas.

### 6.10 Contrato de la vista de datos de tabla (H11)

Decisiones del 2026-11-20 (tomadas por el agente en modo autónomo; pendientes de revisión en el PR):
- **Se amplía la pestaña de datos de H2**, no se crea otra: abrir una tabla (doble clic / Enter / "Open data") ya abre una pestaña de datos con el mismo `QuarryGrid`, `ResultTableModel` y `TableEditModel` que el grid de resultados. H11 le añade orden y filtro en el servidor, página de 200 filas y, en fases posteriores, el resto (ROADMAP H11).
- **Orden y filtro en el servidor** con `TableQueryBuilder` (core, `table_query.h`): `SELECT [oculta,] * FROM t [WHERE …] [ORDER BY c ASC|DESC]`. Identificadores con `quoteIdentifier`/`qualifiedName`; **los valores siempre como parámetros** (`Statement::parameters`), nunca en el texto. Los filtros se unen con `AND`. Operadores: `=`, `<>`, `<`, `<=`, `>`, `>=`, `IS NULL`, `IS NOT NULL`, contiene y empieza por (`LIKE ? ESCAPE '!'`, con `%`, `_` y `!` escapados con `!` en el valor; `!` evita `\`, que MySQL interpreta dentro de los literales). Sin cambios en la ABI de los plugins.
  - Los valores viajan como texto y el servidor convierte según el tipo de la columna, como en la edición (§6.4). La sensibilidad a mayúsculas de `LIKE` es la del motor (SQLite y MySQL no distinguen en ASCII; PostgreSQL sí).
  - Una vista nueva lanza un plan nuevo, que cancela el anterior; Esc / Mod+. lo cancela como cualquier consulta. Con cambios pendientes o un guardado en vuelo, `QueryController::setDataView` se niega y lo anuncia (no se pierden cambios sin querer).
  - La vista se conserva al refrescar (F5) y se pierde al cerrar la pestaña (las pestañas de datos no se guardan, §6.4).
- **Filtro como condición SQL (decidido por el usuario, 2026-11-20):** la barra de filtros de la pestaña de datos es un único cuadro de texto con la condición, como en DBeaver (`edad > 30 AND (ciudad = 'Madrid' OR ciudad = 'Sevilla')`), con `AND`, `OR` y paréntesis. `TableQuery::whereExpression` la lleva; `TableQueryBuilder::validateWhere` la comprueba y se envuelve en `WHERE ( … )`, unida con `AND` a los `ColumnFilter` (que quedan para la navegación por claves foráneas). Es **la excepción** a "los valores siempre como parámetros": el texto es SQL que el usuario escribe contra su propia base. `validateWhere` rechaza fuera de literales e identificadores entre comillas `;`, comentarios, `?` y `$<dígito>` (chocarían con los parámetros), paréntesis sin pareja, y en cualquier parte comillas sin cerrar y NUL; no es un parser, el servidor decide la sintaxis. Un error de validación o del servidor se muestra bajo el cuadro y no cambia la vista.
- **Página de 200 filas** en las pestañas de datos (`ResultTableModel::kDataPageRows`, por instancia con `setPageRows`); el grid de resultados sigue con 500. "Cargar más" es el mismo mecanismo de H1.7: el grid pide la página siguiente al acercarse al final, y "Fetch all" sigue disponible.
- **Conteo** (`TableQueryBuilder::count`: `SELECT COUNT(*)` con los mismos filtros, sin orden) está en core; la acción de la UI llega en la fase siguiente, con una sesión propia para no cortar el stream de la pestaña.
- **Exportar** una pestaña de datos exporta **la vista activa** (fase 2, hecha): `ExportController` pide a `QueryController::viewQueryOf(tabId)` el `TableQuery` de la pestaña (tabla, filtros, condición y orden; **sin** columna oculta y **sin** límite de página) y lo construye con `TableQueryBuilder::select`, que devuelve un `Statement` con `parameters`; `Exporter::start(session, Statement)` lo ejecuta en una sesión propia. Sin vista es la tabla entera. `ExportController::infoFor` añade `isData`, `viewActive` y `queryNote` (el texto que el diálogo enseña). Las ediciones pendientes no se incluyen ("Current result" sigue siendo lo cargado).
- **Estructura y FKs (fase 4, hecha; cambia el contrato del `shortcut:` anterior):** `describe()` ya está en `IConnection` y en `Session` (`described` / `describeFailed`, `TableDetails` con columnas, índices, `ForeignKeyDef` y clave primaria) y el DDL lo **reconstruye** `quarry::DdlBuilder::build(dialect, table, details, isView)` en core (aproximado: no lleva CHECK, triggers, ENUM, intercalaciones ni opciones del motor, y su primera línea lo dice); no hay DDL por provider. La app lo usa así:
  - **`StructureController`** (`src/app/ui/structure_controller.{h,cpp}`, QML `StructureController`, hijo de `MainWindow` después de `ConnectionController`): `load(tabId)` / `reload(tabId)` piden `Session::describe` de la tabla de una pestaña de datos o de estructura sobre la **sesión de metadatos del árbol** (`SchemaTreeModel::ensureSession` / `metadataSession`, nunca la de la pestaña, para no tocar el stream del grid; si la conexión se está abriendo espera con un temporizador de 300 ms hasta 30 s y no pregunta contraseñas). Asíncrono y **cacheado por pestaña** (el estado va por `tabId`; se poda al cerrar la pestaña); un fallo es un estado (`failed`, mensaje y detalle) y no molesta a nadie más. `infoOf(tabId)` entrega columnas, índices, claves foráneas y DDL como `QVariantMap` para QML; `linkColumns(tabId)` dice qué columnas son clave foránea.
  - **`goToRelated(tabId, row, column)`** construye los filtros `{column: <referenciada>, op: "equals", value}` (uno por columna de la clave, valores como parámetros), comprueba la tabla destino con otro `describe` (si falla: `relatedFailed`, ninguna pestaña) y la abre con `EditorTabsModel::openData`; **`QueryController::presetDataView`** hace que esa pestaña nazca ya con el filtro (sin una primera lectura de la tabla entera) y `setRelatedView` reutiliza una pestaña ya abierta; el texto "Filtered from …" es `QueryState::filterOrigin` y se borra cuando otro `setDataView` cambia los filtros. Un valor NULL no abre nada.
  - **`QueryController::cycleSort`** (orden desde la cabecera, fase 2) es `setDataView` con el ciclo ascendente → descendente → ninguno; hereda su rechazo con cambios pendientes.
  - **Pestaña `structure`** en `EditorTabsModel` (`openStructure`, `isStructure`, `objectPartsOf`; `TabKindRole` = `"structure"`), como las de datos y migración: no se guarda en `quarry.db`.
  - **Visores** (`ValueViewer`, QML singleton, `src/app/ui/value_viewer.{h,cpp}`): formatean JSON (sangrado conservando el orden de las claves), XML, texto y el volcado hex de un BLOB, con topes (`kMaxChars` 200 000 caracteres, `kMaxBlobBytes` 64 KiB). `TableEditModel::recordFields` / `fieldView` / `copyField` dan al panel de registro los campos de una fila con las ediciones pendientes, leyendo como mucho 64 K caracteres de un valor (`ResultTableModel::textAt` / `bytesAt` no copian valores enteros).

### 6.11 Contrato de la integración con migraciones (H12)

Decisiones del 2026-11-20 (tomadas por el agente en modo autónomo; pendientes de revisión en el PR). Confirmadas por el usuario (2026-11-20): leer la tabla con la sesión de metadatos del árbol (cancelar descarta la respuesta), la vista en un diálogo y, si la tabla no existe, todo pendiente con un aviso.
- **Solo lectura hasta la fase C del §6.12** (el usuario decidió el 2026-11-21 que ejecute con `php artisan`): hasta entonces, Quarry lista los nombres de los ficheros de la carpeta y hace un `SELECT` a la tabla de migraciones. Nunca ejecuta, revierte ni modifica migraciones, ni ejecuta código del proyecto.
- **Adaptador** `IMigrationSource` (core, `migration_source.h`): `id()`, `displayName()`, `usesTable()`/`defaultTable()`, `validate(settings)` (sin disco), `scanFolder(folder, token)` (hilo de trabajo), `appliedQuery(settings)` (columna 0 = nombre, 1 = lote) y `reconcile(files, applied)` → `MigrationReport` (entradas `Pending`/`Applied`/`Orphan` con lote, totales y `tableMissing`). `migrationSources()` / `findMigrationSource(id)` son el registro: añadir Django, Rails o Flyway es implementar la interfaz y añadirla ahí, sin tocar el resto.
- **Laravel** (`LaravelMigrationSource`, privado de core): ficheros `*.php` de la carpeta, sin recursión (como `php artisan migrate`); nombre = fichero sin `.php`, comparado con `migration`; orden por nombre, que es cronológico por el prefijo `yyyy_mm_dd_hhmmss`. Tabla por defecto `migrations`; el nombre solo admite `identificador` o `esquema.identificador` (letras, dígitos, `_`), así va sin comillas al SQL: sin inyección y con el plegado de mayúsculas de cada motor, como las consultas de Laravel. Filas duplicadas cuentan una vez.
- **Configuración local por conexión** en `quarry.db`, tabla propia `migration_settings(connection_id PK → connections ON DELETE CASCADE, kind, folder, table_name, updated_at)` (migración 5). No está en `ConnectionProfile`, así que nunca viaja con la conexión (exportar conexiones no la incluirá) y nunca está en el repositorio del proyecto.
- **`MigrationService`** (core, `migration_service.h`): fachada del hilo de UI con un hilo de trabajo para `quarry.db` y los escaneos. `inspect(session, settings)` escanea la carpeta en ese hilo y lee la tabla con la **sesión de metadatos del árbol** (no abre otra conexión ni pide contraseñas); vacía el stream con `fetchMore` y cruza. Ignora las señales de la sesión anteriores a su `started()`. Tabla inexistente (`NotFound`, SQLSTATE `42P01`/`42S02` o el texto del driver) no es un error: informe con `tableMissing` y todo pendiente. Carpeta inexistente, relativa, que no es carpeta o ilegible: `Error` claro. Sin sesión abierta: "Connect to the database…".
  - Cancelar corta el escaneo y descarta la respuesta. `shortcut:` no llama a `Session::cancel()`, que también cortaría las peticiones del árbol en esa sesión compartida; si la tabla fuera lenta de leer, se pasaría a una sesión propia.
- **App**: `MigrationController` (ajustes del diálogo de conexión, validación asíncrona de la carpeta, `pendingCounts` por conexión e inspección al conectar) y la lectura del código de una migración (`readSource`, `sourceOf`). En el diálogo de conexión, sección "Project migrations" (tipo, carpeta, tabla) que no deja guardar con una ruta inválida; en el árbol, "N pending migration(s)" como texto junto a la conexión y en su descripción accesible, y (fase B) la carpeta Migrations (hermana de Tables) y la pestaña de migración (el diálogo de lista y su `MigrationListModel` se retiraron).

### 6.12 Árbol por bases de datos, buscador y migraciones ejecutables (H12, fases A a C)

Decisiones del usuario del 2026-11-21 (peticiones literales en el ROADMAP). Tres fases, cada una con su CI, su commit y su visto bueno.
- **Árbol de objetos v2 (fase A).** Los motores con varias bases (PostgreSQL, MySQL/MariaDB, SQL Server, ODBC genérico) muestran bajo la conexión dos carpetas: **Databases** y **Users**. Bajo Databases va cada base; al desplegarla, tres carpetas: **Tables**, **Views** (con las vistas materializadas) e **Indexes**. SQLite (una sola base) no tiene la carpeta Users ni la capa de la base y muestra Tables, Views e Indexes directamente bajo la conexión, como hoy oculta `main`. Las carpetas son nodos virtuales del árbol (`SchemaTreeModel`) que agrupan por `ObjectKind` lo que ya devuelve `children()`; **los `ObjectPath` de las tablas y vistas no cambian** (abrir datos, `reveal`, completado y exportación siguen igual). Lo nuevo viene de los providers: **Indexes** y **Users** se listan con dos kinds nuevos (`ObjectKind::User`, y los índices de la base entera, `ObjectKind::Index` con ruta `{base, [esquema,] tabla, índice}`) pedidos con `children()` sobre carpetas con claves reservadas (`Users`, `Indexes`). Los usuarios son solo de lectura: nombre y, en `detail`, lo que cada motor dé sin privilegios especiales (si la consulta falla por permisos, la carpeta lo dice con un nodo de error y no rompe el resto). Cambio de contrato de providers: se acuerda en una cabecera antes de lanzar a los subagentes (como en H2 y H11).
- **Contrato de providers de la fase A** (cabeceras `object_path.h` y `provider_api.h`): `ObjectKind::User`, `Capability::Users` y dos peticiones nuevas a `IConnection::children()`: la carpeta `{Folder, ["Users"]}` (usuarios o roles del servidor; solo si el provider tiene `Capability::Users`) y la carpeta `{Folder, <padre de las relaciones> + ["Indexes"]}` (todos los índices de las tablas de esa base o de ese esquema, con la misma forma de ruta que la carpeta "Indexes" de cada tabla). Las carpetas **Databases**, **Users**, **Tables**, **Views** e **Indexes** del árbol las pone la app. No cambia `apiVersion`: un plugin viejo no anuncia `Users` y devuelve vacío para `Indexes`. Usuarios por motor: PostgreSQL `pg_roles` (todos pueden leerlo), MySQL/MariaDB `mysql.user` si hay permiso y, si no, los `GRANTEE` distintos de `information_schema.USER_PRIVILEGES`, SQL Server `sys.server_principals` (S, U, G) o, sin permiso, `sys.database_principals`; SQLite y ODBC genérico no tienen usuarios.
- **Buscador del árbol (fase A).** Un campo arriba del panel izquierdo con un botón desplegable que dice por qué se filtra; la opción elegida es el placeholder del campo. Filtros: **Filter by connections**, **Filter by tables and indexes**, **Filter by migrations** y, además, **Filter by everything** (el valor por defecto: cualquier nodo visible), **Filter by views**, **Filter by users** y **Filter by databases**. El filtro es insensible a mayúsculas, con retardo de 150 ms, no carga nodos que no estén cargados (no abre conexiones ni lanza consultas: filtra lo que el árbol ya conoce y lo dice con un texto "N results; connect or expand to search more") y mantiene visibles los padres de cada coincidencia. Esc limpia el campo, y el filtro se anuncia con Accessible.announce.
- **Migraciones en el árbol (fase B).** Si la conexión tiene migraciones configuradas, justo debajo de **Tables** (hermana suya, a la misma profundidad) aparece una carpeta **Migrations** con dos carpetas, **Pending** (ficheros sin fila en la tabla de migraciones) y **Applied** (con su lote en `detail`); las huérfanas van en una tercera carpeta **Orphans** solo si existen. Se quita "Migrations…" del menú contextual y el `MigrationsDialog` actual pasa a ser esta carpeta. Abrir una migración (Enter o doble clic) abre una pestaña de **solo lectura** en el centro con el código PHP, como una pestaña de datos o de consulta, resaltado con KSyntaxHighlighting (definición `PHP`), y arriba una barra con los botones de la fase C (en la fase B sin función, deshabilitados con el motivo).
- **Contrato de la fase B** (cabeceras `migration_source.h` y `migration_service.h`): `IMigrationSource::filePath(settings, name)` (la ruta del fichero de una migración; vacía si el nombre lleva separadores, `..` o NUL: un nombre de la base o de la UI nunca llega a otra ruta), `IMigrationSource::highlightingDefinition()` ("PHP") y `MigrationService::readSource(settings, name)` con las señales `sourceRead` / `sourceFailed`: lectura de solo lectura en el hilo de trabajo, un fichero regular de hasta 1 MiB (más es un error, no se trunca). En el árbol, `SchemaTreeModel` pone una carpeta virtual **Migrations** **debajo de Tables y a su misma profundidad** (hermana, no hija; el usuario lo corrigió el 2026-11-21: antes era un hijo de Tables) con **Pending** y **Applied** (y **Orphans** solo si hay), alimentada por el último `MigrationReport` de la conexión (el que ya calcula `inspect` al conectar), con la hoja de cada migración (icono y texto, el lote en el texto secundario); sin informe todavía, la carpeta lo dice ("Not read yet") y no lanza nada; se refresca cuando llega un informe. Abrir una hoja (Enter o doble clic) abre una pestaña de **migración** de solo lectura, sin guardar en `quarry.db`, en la región del editor: el código en un `SqlEditor` de solo lectura con la definición PHP y, arriba, la barra con **Run**, **Rollback** y **Dry run** (en la fase B deshabilitados, con el motivo "Running migrations arrives in the next step"). Una huérfana no tiene fichero: abrirla muestra un aviso en vez de código.
- **Contrato de la fase C** (cabecera `migration_runner.h`, y `phpPath` y `projectRoot` en `MigrationSettings`, columnas nuevas de `migration_settings` en la migración 6 de `quarry.db`): `MigrationRunner` (core, solo QtCore: `QProcess`) con `resolve(settings)` (php y raíz del proyecto con `artisan`), `arguments(settings, name, action)` (puro, testeable), `resolveTarget` (`php artisan db:show --json`, solo lectura), `execute(settings, name, action)` con las señales `started`, `output(id, text, isError)`, `finished(id, exitCode)`, `cancelled` y `failed`, `cancel(id)`, y la función pura `sameDatabase(ArtisanTarget, ConnectionTarget, &why)`. Las acciones son `Run` (`migrate --path=<fichero> --force`), `Rollback` (`migrate:rollback --path=<fichero> --force`; Laravel solo revierte lo que está en el **último lote**, y la UI lo dice) y `DryRun` (`migrate --path=<fichero> --pretend`; no cambia nada). Reglas: sin shell, lista de argumentos, directorio de trabajo = raíz del proyecto, entorno de la app más `NO_COLOR=1` y `TERM=dumb`, sin contraseñas, salida de hasta 1 MiB, tiempo máximo de 300 s y cancelable. La UI: `Run` y `Rollback` piden **antes** `resolveTarget` y se **bloquean** si `sameDatabase` falla (se enseñan las dos bases); `DryRun` lo hace también pero solo **avisa** si no coinciden. Confirmación siempre antes de `Run` y `Rollback`; en conexiones marcadas como producción hay que **escribir el nombre de la base**. Al terminar `Run` o `Rollback` se vuelve a leer el informe (Pending y Applied). La salida va a Messages de la pestaña de migración. Pruebas con un `php` falso (un ejecutable de test) que registra los argumentos que recibe: **nunca se ejecuta una migración de verdad ni PHP real en los tests ni durante el desarrollo.**
- **Ejecutar, rollback y dry-run (fase C).** Cambia la regla de solo lectura de §6.11 (que el usuario decidió dejar atrás): los tres botones llaman a **`php artisan`** en la carpeta del proyecto. Run = `migrate --path=<fichero relativo> --force`; Rollback = `migrate:rollback --path=<fichero relativo> --force` (Laravel revierte los lotes en los que está ese fichero); Dry-run = `migrate --path=<fichero relativo> --pretend` (muestra el SQL y **no cambia nada**). Se añaden a la configuración de migraciones de la conexión la **ruta de PHP** (por defecto `php` del PATH) y la **raíz del proyecto** (por defecto, dos niveles sobre la carpeta de migraciones). Salvaguardas: (1) antes de cada ejecución real se comprueba **a qué base apunta artisan** (`php artisan db:show --json`, solo lectura) y se **bloquea** si el host y la base no coinciden con la conexión de Quarry, para no migrar otra base por un `.env` distinto; (2) confirmación siempre, y en conexiones marcadas como producción, confirmación escrita del nombre de la base; (3) el dry-run no pide confirmación; (4) se ejecuta con `QProcess` sin shell, con la lista de argumentos, cancelable, fuera del hilo de la UI, con tiempo máximo y sin enviar nunca credenciales por la línea de comandos (artisan lee el `.env` del proyecto); (5) la salida (stdout y stderr) va al panel de mensajes y, al terminar, se vuelve a cruzar la carpeta con la tabla para refrescar Pending y Applied. Ejecuta código del proyecto del usuario: se dice en el diálogo de confirmación. Sin PHP o sin proyecto válido, los botones se deshabilitan con el motivo.
- **Ajustes por base de datos (2026-11-21).** Petición del usuario: con una conexión sin `database` fija, la lectura de `migrations` fallaba ("No database selected") y cada base de un servidor puede ser un proyecto distinto. `MigrationSettings` gana `database` (vacío = la base propia de la conexión: SQLite o `database` fija del perfil) y `migration_settings` pasa a clave `(connection_id, database)` (migración 7 de `quarry.db`, conserva las filas con `database=''`). La tabla se cualifica con la base al leerla: MySQL/MariaDB `` `db`.migrations ``, SQL Server `[db]..migrations` (el esquema por defecto del login elige la tabla, como la consulta de Laravel); PostgreSQL, SQLite y ODBC no cualifican (la base no cambia dentro de una conexión; `database` solo forma parte de la clave). `MigrationService::inspect` y `IMigrationSource::appliedQuery` reciben el dialecto y el id del motor, porque `Session` no los expone; el nombre de la base pasa por `quoteIdentifier`. App: la carpeta Migrations cuelga de cada Tables cuya base tiene ajustes (ya no hay "la primera base cargada"), "Migrations…" en el menú de cada base abre `MigrationSettingsDialog`, y la sección "Project migrations" del diálogo de conexión solo se muestra con base fija o SQLite. Límite: en motores con esquemas, cada Tables de la base muestra Migrations.
- **Riesgos:** el cambio de contrato de providers (usuarios e índices en cinco motores, solo SQLite en local; el resto se ve en la CI con servidores); los permisos para listar usuarios; el tamaño del árbol con miles de índices (carga bajo demanda por carpeta); `php artisan` no se puede probar en la CI sin PHP (se prueba con un `php` falso, un script que imprime lo esperado).

### 6.13 Integración de Claude Code (H13)

Decidido por el agente en modo autónomo (2026-11-22), pendiente de revisión. Plan por fases en el ROADMAP.
- **Módulo aislado `quarry_ai`** (`src/ai/`, namespace `quarry::ai`): enlaza **solo `Qt6::Core`**, nunca `quarry_core` ni `quarry_provider_api`, así que no puede ver `ResultBatch`, `Session` ni ejecutar nada. Lo comprueban `quarry_assert_ai_isolated` (CMake) y `tst_ai_architecture` (includes, nombres prohibidos y enlace). La app lo enlazará solo en la fase B; con la IA desactivada no se crea ningún objeto ni proceso.
- **Entrada única: `SchemaContext`** (`schema_context.h`): dialecto y tablas con columnas, tipos, nulabilidad, PK, FK e índices. Sin ningún campo para filas o celdas. Comentarios, valores por defecto, CHECK, valores de ENUM/SET (también los que vienen dentro del tipo, `enum('a','b')` → `enum(…)`) y el código de vistas o rutinas solo salen con su opción de `ContextOptions` (todas desactivadas por defecto). La app lo llenará desde la introspección (`describe`), nunca desde resultados.
- **`ContextBuilder`**: tablas del script, de la petición (nombre, plural o singular) y a un salto de FK, hasta `maxTables` (40; primero las del script); esquemas y tablas excluidos no aparecen ni como destino de una FK. Salida en DDL compacto, una línea por elemento y sin caracteres de control (un comentario no puede abrir líneas nuevas ni cerrar el bloque). Caché de 32 entradas por (script, petición, opciones), que se vacía al cambiar el esquema.
- **Saneamiento** (`ai_sanitizer.h`): `sanitizeErrorMessage` quita textos entre comillas simples, `Key (c)=(v)`, `row contains (…)`, `value is (…)` y números sueltos. `maskLiterals` sustituye literales de texto (`''`, `N''`, `E''`, dollar-quoted) y, opcionalmente, números por `__V1__`, `__V2__`…; `unmaskLiterals` los repone en lo que devuelve la IA. No toca comentarios ni identificadores entre comillas.
- **Prompt** (`ai_prompt.h`): instrucciones fijas en `--system-prompt`; todo lo demás va por **stdin** en bloques `<quarry-…>` declarados como datos no fiables, con los cierres `</quarry-` neutralizados. Cada ejecución es independiente: el historial se reconstruye de lo más nuevo a lo más viejo hasta `maxChars` (60 000) y se dice cuántos mensajes se omiten. La IA propone cambios devolviendo el script completo entre `<quarry-script>` y `</quarry-script>`; no tiene herramientas para tocar ficheros.
- **CLI** (`claude_cli.h`): argumentos fijos `--print --output-format json --input-format text --no-session-persistence --tools "" --disallowedTools <todas las integradas> --strict-mcp-config --setting-sources "" --settings {"disableAllHooks":true} --disable-slash-commands --system-prompt <fijo>`; nunca texto del usuario en la línea de órdenes. Flags verificados con `claude --help` (ver la viñeta de flags verificados); falta ver la salida real. Rutas candidatas por sistema (en Windows, ejecutables nativos antes que el `.cmd` de npm) y, en Unix, una consulta única a `$SHELL -l -c "command -v claude"`. Versión mínima 2.0.0. `parseClaudeOutput` clasifica: sin sesión (pista `claude /login`), límite de uso, versión incompatible (opción desconocida), salida no válida o vacía, y fallo genérico; CLI no encontrado, timeout y cancelación los pone el ejecutor.
- **Diff** (`script_diff.h`): diff por líneas (LCS; por encima de 4 M celdas, un solo bloque) entre el script y la copia de la IA. `result()` aplica solo los bloques aceptados: el original no cambia hasta aceptar. Aceptar o rechazar por bloque o todo, con deshacer de cada acción. `modifiesData` marca (a propósito, de más) los bloques con DML o DDL para avisar antes de ejecutar; al ejecutar se aplican las confirmaciones de siempre (`SqlSafety`), que viven en core y la IA no puede llamar.
- **Contrato de la fase B** (decidido con el usuario, 2026-11-22; cabeceras `claude_runner.h` en `quarry_ai` y `ai_settings.h` en core): (1) **`ClaudeRunner`** (`quarry_ai`, solo Qt6::Core, asíncrono con `QProcess`, sin hilo propio): `locate` (ruta propia, rutas candidatas y, en Unix y solo si no hay ninguna, una consulta única al shell de login; con caché), `check` (`claude --version`, no gasta nada) y `ask` (argumentos fijos `claudeArguments()`, el texto solo por stdin, directorio de trabajo = una carpeta temporal **vacía** para que no se lea ningún CLAUDE.md ni `.claude/` del sitio desde el que se lanzó la app, entorno de la app más `NO_COLOR` y `TERM=dumb`, salida de hasta 8 MiB, tiempo máximo de 120 s por defecto y cierre del árbol de procesos: Job Object en Windows, grupo de procesos en Unix). Devuelve siempre un `CliOutcome` (sin sesión, límite de uso, versión incompatible, salida corrupta, tiempo, cancelado, CLI ausente). El cierre del árbol está escrito otra vez porque `quarry_ai` no puede enlazar core (`// shortcut:`). (2) **Ajustes por conexión** (`AiConnectionSettings` y `AiSettingsService`, tabla `ai_settings` de `quarry.db`, migración 8, con borrado en cascada y fuera de `ConnectionProfile`): interruptor (**desactivado por defecto en todas las conexiones**; activarlo en una de producción pide además una confirmación escrita), exclusiones de esquemas y tablas, opciones de metadatos, enmascarar literales y números y máximo de tablas. (3) **Ajustes globales**: la ruta del CLI (`ai/cliPath` en las preferencias de la UI, vacía = buscar) y el botón **Test** en el diálogo de Ajustes, sección "AI assistant". (4) **Adaptador de la app** que llena `SchemaContext` desde la introspección que la app ya tiene (`describe` y el árbol), nunca desde resultados, y la sección "AI assistant" del diálogo de conexión. **Nunca se ejecuta el `claude` real en tests ni durante el desarrollo**: se prueba con un ejecutable `fake_claude`.
- **Flags de `claude -p` verificados (2026-11-22, con el usuario, solo `claude --help` y `--version`; nunca una petición):** Claude Code 2.1.319. Existen y están documentados en `--help`: `--print`, `--output-format`, `--input-format`, `--no-session-persistence`, `--tools` (con "" desactiva todas), `--disallowedTools`, `--strict-mcp-config`, `--setting-sources`, `--settings`, `--disable-slash-commands` y `--system-prompt`. **`--max-turns` no aparece en `--help`** y no se puede comprobar sin una petición real (el CLI acepta opciones desconocidas con `--version`): se **quita** de `claudeArguments()`, porque con todas las herramientas desactivadas ya hay un solo turno. **`--bare` no se usa:** salta los hooks y las demás cosas del usuario, pero en ese modo no se lee la suscripción (solo `ANTHROPIC_API_KEY`), y el chat usa la suscripción. La forma real de `--output-format json` y de los errores de sin sesión o límite sigue sin verse (haría falta una petición real: la prueba la hace el usuario a mano).
- **Contrato de la fase C (decidido con el usuario, 2026-11-22):** (1) `Session::describe(table)` asíncrono en core (`described` / `describeFailed`, como `children`), que usa `AiSchemaAdapter` para dar el contexto completo. (2) El chat es un **panel lateral a la derecha del editor, por pestaña de consulta** (usa la conexión de esa pestaña), con botón y comando para abrirlo y cerrarlo. (3) Flujo de una petición: contexto (adaptador) → `ContextBuilder::build` → enmascarar literales del script y del historial según los ajustes → `buildPrompt` → `ClaudeRunner::ask` → `parseReply` → desenmascarar → `ScriptDiff` contra el script. La IA nunca ejecuta nada: aceptar cambios solo edita el texto del script, y ejecutarlo pasa por las confirmaciones de siempre. (4) Estados pensando, error y terminado, **Cancel**, errores claros (sin sesión con el comando de inicio de sesión, límite de uso, versión incompatible, CLI ausente) con el texto de la IA saneado y **Details** copiable. (5) Panel **"Context sent"**: lo enviado en cada petición (tablas, DDL y el script ya enmascarado); en memoria por pestaña y, solo si el usuario lo activa, en un registro local. (6) Diff en el editor: bloques marcados "Generated by AI" hasta aceptarlos o rechazarlos (por bloque o todo, con deshacer), aviso si un bloque tiene DML o DDL, nada solo por color, anuncio a lectores de pantalla. (7) Aviso fijo de que se usa la suscripción del usuario (o su clave de API) y que Quarry no envía nada por su cuenta.
- **Fase C, hecho en la app (2026-11-22):** `AiChatController` (uno por pestaña de consulta, hijo de `AiController`, que lo crea al abrir el panel y lo destruye con la pestaña), `AiScriptDiff` (modelo de bloques sobre `ScriptDiff`), `AiRecordModel` (mensajes y "Context sent", en memoria), y en QML `AiChatPanel` y `AiDiffReview`, enlazados desde `EditorArea`. `AiController` guarda además la casilla del registro local (`ui/layout/ai/keepLog`) y escribe `ai-requests.log` (rotado, en un hilo del pool). El adaptador usa `Session::describe`. La IA sigue sin tocar resultados ni ejecución: el único texto del editor que lee es el script de la pestaña y el único dato de una ejecución es el mensaje de error de la última, saneado. Detalle de la interfaz en UX.md (fase C).

### 6.14 Dump y restore (H14)

Decidido por el agente en modo autónomo (2026-11-22), pendiente de revisión. Plan por fases en el ROADMAP; la fase A (core) está hecha.
- **Dónde vive.** En **core**, no en la API de providers: los motores se eligen por `providerId` (`dumpSupported`: `sqlite` y `postgres`), igual que `IMigrationSource` elige el framework. Descartado ampliar `IConnection`/`apiVersion`: un plugin no debe lanzar procesos y el dump de PostgreSQL no usa la conexión abierta. Cabeceras `dump.h` (tipos y funciones puras) y `dump_runner.h` (`DumpRunner`). Un motor nuevo (MySQL con `mysqldump`, SQL Server…) añade su rama en `DumpRunner` y sus funciones de argumentos.
- **Opciones.** `DumpOptions`: contenido (`SchemaAndData`, `SchemaOnly`, `DataOnly`), formato (`PlainSql`; `PostgresCustom` solo en PostgreSQL), esquemas (PostgreSQL), tablas (vacío = todas), ruta de salida y `includeOwnership` (por defecto `--no-owner --no-privileges`, para restaurar en otro servidor). `RestoreOptions`: el fichero. `DumpTarget` se construye con `dumpTargetFromProfile(perfil, contraseña, base)`.
- **Tablas relacionadas.** `dependencyClosure(seleccionadas, aristas)`: añade, transitivamente, las tablas a las que **apuntan** las FK de las seleccionadas (nunca las que las referencian), con ciclos y autorreferencias. Las aristas las saca la app de `Session::describe` (fase B).
- **PostgreSQL.** `pg_dump` (`--dbname=<conninfo>` sin contraseña, `--no-password`, `--verbose`, `--format=plain|custom`, `--schema-only`/`--data-only`, `--schema="s"` y `--table="s"."t"` entre comillas dobles para que el nombre sea literal) escribe en `<salida>.part`, que se renombra solo si termina con código 0. Restore: un fichero que empieza por `PGDMP` va a `pg_restore --exit-on-error --single-transaction --no-owner`; uno de texto a `psql --no-psqlrc --set=ON_ERROR_STOP=1 --single-transaction --echo-errors`. Errores con mensaje propio: versión del cliente antigua, autenticación y conexión; el resto, "<herramienta> failed with exit code N" con las últimas 20 líneas de stderr en `detail`.
- **Credenciales.** La contraseña nunca va en la línea de órdenes ni en `PGPASSWORD` (que `ps` puede enseñar y que se quita del entorno heredado): se escribe en un `PGPASSFILE` temporal (`quarry-pgpass-XXXXXX`, solo el propietario, `*:*:*:*:<contraseña escapada>`), que se borra con el proceso. Con túnel SSH se rechaza (`Unsupported`): las herramientas no pasan por el túnel de Quarry.
- **Herramientas.** `resolveDumpTool(tool, DumpToolPaths)`: la ruta configurada (programa o carpeta) o, si está vacía, el PATH y las carpetas habituales (Windows `Program Files/PostgreSQL/<versión>/bin`, la más nueva primero, y `pgAdmin 4/runtime`; macOS libpq de Homebrew y Postgres.app; Linux `/usr/lib/postgresql/<versión>/bin` y `/usr/pgsql-<versión>/bin`). Error claro que dice qué programa falta y dónde configurarlo.
- **SQLite.** En proceso con la biblioteca sqlite3 que core ya enlaza (no hace falta el programa `sqlite3`): el mismo guion que `.dump` (`PRAGMA foreign_keys=OFF`, una transacción, cada tabla y sus filas con la lista de columnas sin las generadas, luego índices, triggers y vistas, y `sqlite_sequence`), leído en una sola transacción de lectura y escrito con `QSaveFile`. Con una selección de tablas se incluyen sus índices y triggers y **no** las vistas. En "solo datos" los triggers de esas tablas se borran antes de las filas y se vuelven a crear después, en la misma transacción (SQLite no puede desactivarlos; equivale a `--disable-triggers`). Restore: sentencia a sentencia como `.read` (`sqlite3_complete`); si falla o se cancela con la transacción del guion abierta, `ROLLBACK` y el mensaje dice si la base quedó como estaba. Se ejecuta en un hilo propio con un token de cancelación (`sqlite3_interrupt`).
- **Ejecución.** `DumpRunner` vive en el hilo de la UI y no lo bloquea (QProcess asíncrono, sin shell, entorno de la app más `PGCLIENTENCODING=UTF8`, árbol de procesos cerrado al cancelar con Job Object o grupo de procesos). Señales `started`, `progress(id, hecho, total, paso)` (total 0 = indeterminado: PostgreSQL), `output(id, línea, esError)` (hasta 5000 líneas) y una de `finished`, `cancelled` o `failed(id, Error)`. Un dump cancelado o fallido no deja fichero.
- **Fase B (app, pendiente):** menú contextual de una base → **Tools** → submenú **Dump…** / **Restore…**; diálogo de dump (conexión, contenido, formato, fichero, selección de tablas con buscador arriba y casilla "Also select the tables they reference" que usa `dependencyClosure`); diálogo de restore con confirmación explícita (en producción, escribir el nombre de la base) y aviso de que conviene una base vacía; progreso, registro y Cancel sin bloquear; rutas de las herramientas en Ajustes (`dump/pgBinDir`, vacía = buscar); accesible con teclado y lector de pantalla y textos en `quarry_es.ts`.

## 7. Gestión de errores

- Los providers traducen los errores del driver a `Error`: `kind` según SQLSTATE o código nativo; `message` en lenguaje claro y traducible; `detail` con el texto literal del driver; `position` si el driver la da (PostgreSQL la da).
- En la UI, un **panel de mensajes** muestra el `message` destacado, con un icono y un texto que indica el tipo: nunca solo color. El `detail` va en una sección desplegable "Technical details" que se puede copiar. Si hay `position`, se subraya en el editor y la posición se anuncia por accesibilidad.
- Los errores de conexión sugieren qué hacer según el `kind`: Authentication → "Check the user name and password"; Connection → "Check host, port and that the server is running"; Timeout → "The server did not answer in 15 s".
- Los errores internos (bugs) se registran en el log local (`QStandardPaths::AppLocalDataLocation/logs`, rotado, sin datos de consultas por defecto) y se muestran con un genérico "Something went wrong" y un enlace para abrir el log.
- Si un plugin falla al cargar (ABI distinto, falta una DLL del driver), el motor sale como "unavailable" con el motivo, y el resto de la app sigue funcionando.

## 8. Sistema de plugins

- Cada motor es una librería `MODULE` en `<app>/plugins/quarry/` (en macOS, `Quarry.app/Contents/PlugIns/quarry/`), con un JSON embebido:
  ```json
  { "id": "postgres", "name": "PostgreSQL", "version": "0.1.0",
    "apiVersion": 1, "fileExtensions": [], "urlSchemes": ["postgres", "postgresql"],
    "capabilities": ["Schemas", "MultipleDatabases", "Transactions", "Cancel", "Explain"] }
  ```
- **Descubrimiento sin carga:** en el arranque, `ProviderRegistry` recorre el directorio y lee `QPluginLoader::metaData()`, que no carga la librería. Con eso llena el selector de motores de "Nueva conexión". `load()` + `createProvider()` solo se ejecutan cuando se abre una conexión de ese motor.
- **Versionado:** el IID incluye la versión mayor de la API (`/1`). Si cambia de forma incompatible, se sube a `/2`; los plugins viejos se ignoran y se registra el motivo. `apiVersion` en el JSON permite filtrar sin cargar.
- **Restricción:** un plugin Qt tiene que compilarse con la misma versión mayor/menor de Qt y un compilador compatible con el ABI. Los plugins oficiales se compilan en el mismo build. En v1 no se soportan plugins de terceros compilados aparte; se documenta el SDK cuando la API se estabilice.
- **Drivers nativos:** en Windows, libpq, OpenSSL (y más adelante MariaDB Connector/C) van **junto a `quarry.exe`**, no en `plugins/quarry`: el cargador de Windows no busca en la carpeta del plugin y `ProviderRegistry` rechaza cualquier otra DLL que encuentre allí (decisión del 2026-11-15; el build los copia con un POST_BUILD del plugin). macOS: `@loader_path`; Linux: `$ORIGIN` (en el árbol de build, el RPATH de vcpkg). SQLite va compilado dentro de su plugin; la base de datos de la app usa su propia copia enlazada en `core`.
- **Añadir un motor** = nueva carpeta en `src/providers/<id>/`:
  1. `CMakeLists.txt` con `quarry_add_provider(<id> SOURCES … DEPENDS <driver>)`.
  2. Implementar `IDatabaseProvider`, `IConnection`, `IResultStream` y `ISqlDialect`.
  3. Pasar la **suite de conformidad** común, `tests/providers/conformance/`: un conjunto de tests parametrizados (conectar, introspección, tipos, streaming, cancelación, errores) que cada provider instancia con su fixture.

## 9. Dependencias y vcpkg

| Librería | Origen | Uso |
|---|---|---|
| Qt 6.12 | aqt (`C:\Qt`, `~/Qt`) | UI, core |
| sqlite3 | vcpkg | provider SQLite + `quarry.db` |
| libpq 18 | vcpkg | provider PostgreSQL |
| qtkeychain-qt6 | vcpkg + shim | credenciales |
| syntax-highlighting (KF6) | vcpkg + shim | resaltado SQL |
| libmariadb | vcpkg | provider MySQL / MariaDB |
| unixODBC 2.3 | vcpkg (Linux y macOS; `odbc32` del sistema en Windows) | providers `odbc` y `mssql` |
| libssh2 | vcpkg | túneles SSH (H8) |

**Overlay *shim* de Qt.** `vcpkg-overlays/ports/qtbase` y `qttools` son ports vacíos (`VCPKG_POLICY_EMPTY_PACKAGE`) con la versión de `.qt-version`. Los triplets overlay (`x64-windows-quarry`, `x64-linux-quarry`, `arm64-osx-quarry`…) añaden `VCPKG_ENV_PASSTHROUGH QT_ROOT_DIR` y pasan `-DCMAKE_PREFIX_PATH=$ENV{QT_ROOT_DIR}` a los ports. Así QtKeychain y KSyntaxHighlighting se compilan contra la Qt de aqt y no aparece una segunda Qt. Ventajas: un solo gestor de dependencias, caché binaria de vcpkg en la CI y Perl (lo necesita KSyntaxHighlighting) gestionado por vcpkg. `vcpkg.json` fija un `builtin-baseline`.

## 10. Credenciales y seguridad

- **Nunca hay contraseñas en texto plano** en disco, logs ni historial. `ConnectionStore` (`quarry.db`) guarda host, puerto, usuario, opciones, color y la marca de producción, pero no el secreto.
- `CredentialStore` usa QtKeychain: Windows Credential Manager, macOS Keychain, Linux Secret Service (libsecret/KWallet). Clave: servicio `dev.quarry.Quarry`, cuenta `connection/<uuid>/password` (y `…/ssh-passphrase` cuando haya SSH).
- Si el llavero no está disponible (Linux sin Secret Service), se ofrece **"Ask every time"**: la contraseña se pide al conectar y solo vive en memoria durante la sesión. Nunca se guarda en otro sitio como alternativa.
- El usuario elige si guarda la contraseña o la pide siempre. Borrar una conexión borra también su entrada del llavero.
- Las cadenas de conexión que se muestran o copian enmascaran la contraseña. La exportación de conexiones (hito posterior) nunca incluye secretos.
- Seguridad del usuario (se detalla en UX.md): color por conexión; marca de producción en rojo, con icono y texto; confirmación para `UPDATE`/`DELETE` sin `WHERE`; vista previa del SQL antes de aplicar ediciones; deshacer en el editor de datos; modo de solo lectura por conexión (`SET default_transaction_read_only`, `PRAGMA query_only`).

## 11. Base de datos propia (`quarry.db`)

- Ruta: `QStandardPaths::AppDataLocation/quarry.db`. Modo WAL. Migraciones con `PRAGMA user_version`, aplicadas en el arranque dentro de una transacción.
- Tablas iniciales: `connections`, `connection_folders`, `query_history` (con FTS5 en su hito), `saved_queries`, `ui_state` (pestañas abiertas, tamaño de paneles).
- Se accede desde un hilo de `core` propio. La UI nunca espera a disco en el arranque: carga la lista de conexiones de forma asíncrona y muestra el estado vacío o la lista cuando llega, normalmente en < 10 ms.
- Cada hito crea solo las tablas que usa. H1.3 crea `connections` (migración 1). El resto llega con su hito.

### 11.1 Contrato de conexiones y credenciales (H1.3)

Contrato compartido entre `core` (backend) y `app` (frontend). Las cabeceras viven en `src/core/include/quarry/`. Todas las clases son QObject del hilo de UI con API **asíncrona**: el trabajo de disco y llavero va a un hilo de `core` y los resultados llegan por señales con `Qt::QueuedConnection`. Los errores son `quarry::Error` con `message` en lenguaje claro y `detail` técnico.

```cpp
namespace quarry {
enum class PasswordMode : uint8_t { None, Saved, AskEveryTime };

struct ConnectionProfile {
    QUuid id;                       // generado al crear
    QString name;                   // obligatorio y no vacío (lo valida ConnectionStore)
    QString providerId;             // "sqlite", "postgres"…
    QVariantMap values;             // campos de connectionFields() SIN secretos
    int colorIndex = -1;            // -1 = sin color; 0..7 = paleta de UX §6
    bool production = false;
    PasswordMode passwordMode = PasswordMode::None;
    QDateTime createdAt, updatedAt; // UTC, los fija ConnectionStore
    std::optional<QDateTime> lastUsedAt;
};

class ConnectionStore : public QObject {          // tabla `connections`, migraciones con user_version
    explicit ConnectionStore(QString databasePath, QObject *parent = nullptr);
    void load();                                   // -> loaded(QList<ConnectionProfile>) | failed(Error)
    void save(ConnectionProfile profile, const QStringList &secretFieldKeys = {}); // inserta o actualiza -> saved(ConnectionProfile) | failed(Error)
                                                   // secretFieldKeys = claves de los campos Kind::Secret del provider; se eliminan (y siempre "password")
    void remove(QUuid id);                         // -> removed(QUuid) | failed(Error). La app borra también la credencial.
    void touch(QUuid id);                          // actualiza lastUsedAt -> saved(...)
    static QString defaultDatabasePath();          // QStandardPaths::AppDataLocation/quarry.db
};

class CredentialStore : public QObject {           // QtKeychain; servicio "dev.quarry.Quarry", cuenta "connection/<uuid>/password"
    void readPassword(QUuid id);                    // -> passwordRead(QUuid, QString) | failed(QUuid, Error)
    void writePassword(QUuid id, QString password); // -> passwordWritten(QUuid) | failed(QUuid, Error)
    void deletePassword(QUuid id);                  // -> passwordDeleted(QUuid) | failed(...). "Not found" cuenta como éxito.
    // "Ask every time": la contraseña solo vive en memoria durante la ejecución de la app.
    void rememberForSession(QUuid id, QString password);
    std::optional<QString> sessionPassword(QUuid id) const;
    void forgetForSession(QUuid id);
};

class ConnectionTester : public QObject {          // usa ProviderRegistry + Session; timeout 15 s
    explicit ConnectionTester(ProviderRegistry &registry, QObject *parent = nullptr);
    void test(const ConnectionProfile &profile, const Secrets &secrets); // -> succeeded(ServerInfo) | failed(Error); un test a la vez
    void setTimeoutMs(int ms);                                // 15000 por defecto (seam de tests)
    void cancel();                                            // -> cancelled()
};
}
```

Reglas:
- `quarry.db` **nunca** contiene contraseñas, y `values` no puede llevar un campo de tipo `ConnectionField::Kind::Secret` (`ConnectionStore` los elimina antes de guardar).
- Si el llavero no está disponible, `CredentialStore` falla con un `Error` claro ("The system keychain is not available…") y la UI ofrece "Ask every time". **Nunca** hay un respaldo en disco.
- Borrar una conexión: `ConnectionStore::remove` y después `CredentialStore::deletePassword`, que orquesta la app. `deletePassword` solo se llama si el modo era `Saved` (así no se toca el llavero sin necesidad); `forgetForSession` se llama siempre.
- Guardar con `Saved`: primero se escribe en el llavero y después en `quarry.db`. Si el llavero falla, no se guarda nada y el diálogo ofrece cambiar a "Ask every time".
- Duplicar conserva el modo de contraseña pero no copia la contraseña. Abrir un archivo SQLite que ya está guardado selecciona esa conexión en vez de crear otra.
- Implementación (H1.3): `failed(Error)` de `ConnectionStore` no indica la operación; `load()` ordena por nombre; `lastUsedAt` solo lo cambia `touch()`. `ConnectionTester` emite siempre desde el event loop, nunca dentro de `test()`. Tras recargar, los números de `values` pueden ser `double`/`qint64`: usa `toInt()`.
- Fuera de H1.3: carpetas de conexiones, modo de solo lectura e importar/exportar conexiones.

### 11.2 Contrato de pestañas del editor (H1.5)

Las pestañas del editor y su texto se guardan en `quarry.db` para que nunca se pierda lo escrito. Mismo patrón que `ConnectionStore`: QObject del hilo de UI, trabajo en un hilo de `core`, resultados por señales en cola.

```cpp
namespace quarry {
struct EditorTab {
    QUuid id;                           // nulo = nueva (el store genera uno)
    std::optional<QUuid> connectionId;  // nullopt = pestaña sin conexión
    QString title;                      // "Query 1"…; no vacío
    QString text;
    int cursorPosition = 0;
    int position = 0;                   // orden en la barra de pestañas
    QDateTime updatedAt;                // UTC, lo fija el store
};

class EditorTabStore : public QObject {           // tabla `editor_tabs` (migración 2)
    explicit EditorTabStore(QString databasePath, QObject *parent = nullptr);
    ~EditorTabStore() override;                   // termina las escrituras en cola y une el hilo (≤ 5 s)
    void load();                                  // -> loaded(QList<EditorTab>) ordenadas por position | failed(Error)
    void save(quarry::EditorTab tab);             // inserta o actualiza -> saved(EditorTab) | failed(Error)
    void remove(QUuid id);                        // -> removed(QUuid) (también si no existía) | failed(Error)
};
}
```

- `editor_tabs(id, connection_id REFERENCES connections(id) ON DELETE SET NULL, title, text, cursor_position, position, updated_at)`: borrar una conexión deja sus pestañas sin conexión, no las borra.
- Las migraciones viven en un solo sitio de `core` (`AppDatabase`), compartido por `ConnectionStore` y `EditorTabStore`, y son seguras si los dos abren `quarry.db` a la vez (transacción `BEGIN IMMEDIATE`).
- La app guarda con *debounce* (~1 s tras la última tecla) y al cerrar la pestaña o la app; el destructor del store garantiza que esas escrituras terminan.
- En la app, `EditorTabsModel` (`src/app/ui`) guarda la lista, el texto en memoria y la lógica de sentencias: parte el texto con `splitStatements()` del dialecto 250 ms después de la última edición (la sentencia bajo el cursor usa el último corte) y `run()` siempre parte el texto actual. `run()` devuelve el plan `{ok, kind, message, tabId, connectionId, index, count, ranges[{begin, end, startLine, endLine}]}` y emite `runRequested(plan)`: es el punto de enganche de la ejecución en H1.6. Las pestañas se cargan después del primer frame; una pestaña abierta antes se conserva y las guardadas van delante. Al borrar una conexión, sus pestañas pierden la conexión también en memoria.
- `SqlDocument` conecta un `TextEdit` con KSyntaxHighlighting. No se usan archivos `.theme`: un `SyntaxHighlighter` propio colorea cada estilo con los tokens `syn.*` de `Theme` (UX §6), así que el cambio de tema repinta en vivo y el contraste se comprueba en `tst_theme_contrast`. Definiciones: "SQL" sin conexión, "SQL (PostgreSQL)" para PostgreSQL.
- `shortcut:` los números de línea suponen que todas las líneas tienen la misma altura; con fuentes de reserva (CJK, emoji) pueden desalinearse. Mejorarlo si se nota, leyendo la geometría de cada bloque visible.

### 11.3 Contrato de historial y consultas guardadas (H4)

Decisiones del 2026-11-16 (ver ROADMAP H4). Cabeceras en `src/core/include/quarry/`: `history_entry.h`, `history_query.h`, `query_history_store.h`, `saved_folder.h`, `saved_query.h`, `saved_query_store.h`, más `ConnectionProfile::recordHistory`. Mismo modelo de hilos que `EditorTabStore`.

- **Migraciones 3 y 4** (cada hito crea solo lo suyo): `connections.record_history INTEGER NOT NULL DEFAULT 1`; `query_history(id INTEGER PK, connection_id REFERENCES connections ON DELETE CASCADE, sql, executed_at, duration_ms, rows, success, error_message)` con índice por `(connection_id, executed_at)`; tabla FTS5 externa `query_history_fts(sql)` con `content='query_history'` y triggers de insert/delete/update; `saved_folders`; `saved_queries` (`folder_id` y `connection_id` con `ON DELETE SET NULL`).
- **Historial:** lo escribe `QueryController` al terminar cada sentencia (éxito o error; las canceladas no se guardan). `QueryHistoryStore::record` descarta la entrada si la conexión tiene `recordHistory == false`, así la regla no depende de la UI. Tope de 10 000 entradas (`kMaxEntries`); se recortan las más antiguas en la misma transacción. Borrar una conexión borra su historial.
- **Búsqueda:** texto vacío = sin FTS, orden `executed_at DESC, id DESC`. Con texto, cada palabra se entrecomilla (sin operadores FTS del usuario) y la última lleva `*`. Paginación con `limit`/`offset` y `hasMore`.
- **Privacidad:** al desactivar `recordHistory` la app pregunta si borrar el historial existente de esa conexión y, solo si se confirma, llama a `clear(connectionId)`.
- **Consultas guardadas:** carpetas de un nivel; borrar una carpeta mueve sus consultas al nivel superior.
- **Verificación previa:** comprobar que el sqlite3 de vcpkg trae FTS5 (`PRAGMA compile_options`); si no, activar la característica `fts5` del port.

## 12. Arranque rápido

- No se carga ningún provider ni se abre ninguna conexión en el arranque. El escaneo de providers (solo metadatos) y el hilo de `ConnectionStore` arrancan en `ConnectionController::start()`, después del primer frame.
- Los diálogos del sistema (`QtQuick.Dialogs`, para elegir el archivo SQLite) se cargan con un `Loader` por URL al usarse por primera vez, no en el arranque. Es una dependencia de despliegue: H10 debe incluir los DLL de Qt6QuickDialogs2 y su plugin QML. Las vistas QML se crean con `Loader { asynchronous: true }` salvo el esqueleto de la ventana principal.
- QML precompilado con **qmlcachegen** (y `qmlsc` cuando esté disponible), con `qt_add_qml_module`, para evitar compilar QML en tiempo de ejecución.
- KSyntaxHighlighting y su `Repository` (definiciones) se inicializan al abrir el primer editor, no en el arranque. Solo se paga la carga de su DLL al iniciar el proceso (~6 ms): no se puede diferir porque la app enlaza su metaobjeto exportado. Abrir una pestaña de 10 000 líneas cuesta ~170 ms una vez, en el primer resaltado.
- Las medidas y los presupuestos están en [PERFORMANCE.md](PERFORMANCE.md).

## 13. Riesgos técnicos identificados

| Riesgo | Mitigación |
|---|---|
| **Qt Quick no expone `QAccessibleTableInterface`**: un `TableView` no se anuncia como tabla a NVDA, VoiceOver ni Orca | Spike en el Hito 1: `QuarryGrid` (QQuickItem en C++) con un `QAccessibleInterface` propio (`QAccessible::installFactory`) que implementa la tabla, las celdas y las cabeceras sobre el modelo. Plan B: el elemento enfocado anuncia "fila X de N, columna Y: valor" con `Accessible.announce` (Qt 6.8+) |
| Rendimiento de `TableView` con 1 M de filas y anchos variables | Altura de fila fija, anchos de columna calculados con una muestra de las primeras 500 filas, `reuseItems: true`, delegates sin `Loader` |
| QtKeychain en Linux sin Secret Service | Modo "Ask every time" (§10) |
| aqt fijado a un commit de master | Volver a una versión de PyPI cuando salga una > 3.3.0 (`scripts/requirements-aqt.txt`) |
| macOS universal con vcpkg | Compilar dos triplets (arm64/x64) y unir con `lipo` en el hito de distribución |

**Resultado del spike (H1.7, 2026-11-15):** se publica el **plan B**. El cuerpo del grid es un único elemento con rol Table, nombre y descripción (filas, columnas y teclas); las celdas y cabeceras llevan `Accessible.ignored`. Cada movimiento anuncia "Row X of N, <columna>: <valor>" (con un retardo de 120 ms, para que una tecla mantenida solo diga dónde acaba; un rango añade ", N cells selected"). El **plan A** funciona en `tst_grid_accessible.cpp` (una factoría instalada después del primer motor QML, una subclase de `QAccessibleQuickItem` con `QAccessibleTableInterface` y celdas perezosas, probado con 1 M filas), pero necesita la cabecera privada `qaccessiblequickitem_p.h` (`Qt6::QuickPrivate`): con la API pública no se puede, porque pedir la interfaz interior la registra y `QAccessibleCache` falla al registrar otra para el mismo objeto. Decisión: plan B ahora; plan A se reconsidera tras la pasada manual con NVDA/VoiceOver/Orca, aceptando o no el riesgo de API privada. Hallazgo: Qt Quick sigue listando los delegates como hijos "Client" sin nombre aunque lleven `Accessible.ignored`.

## 14. Exportación (H5)

Decisiones del 2026-11-17 (ver ROADMAP H5). Cabeceras en `src/core/include/quarry/`: `export_format.h`, `export_options.h`, `exporter.h`. `core` sigue usando solo Qt6::Core.

- **`Exporter`:** un objeto por exportación, en el hilo de UI. Formatea y escribe en un hilo propio; la `Session` (objeto del hilo de UI) la maneja desde el hilo de UI y solo pasa los lotes al hilo de escritura. Un lote en vuelo: `fetchMore` se pide cuando el anterior está escrito, así la RAM es constante. `start()` hace streaming de una sentencia con una sesión propia ya abierta (el `Exporter` la cierra al terminar); `startBatches()` escribe lotes ya cargados (el resultado actual del grid).
- **Archivo:** se escribe en un temporal hermano y se renombra sobre `filePath` al terminar bien; si falla o se cancela, el temporal se borra y un archivo previo queda intacto.
- **Valores por formato:**
  - CSV: separador configurable, comillas RFC 4180 (si hay separador, comilla, CR o LF), fin de línea `
`, NULL vacío, UTF-8 sin BOM salvo `bom`. TSV: tabulador; los tabuladores y saltos de línea del valor se escapan como `	`, `
` y `
`, y la barra como `\`.
  - JSON: un array de objetos (`[` y `]` en streaming, un objeto por línea); JSON Lines: un objeto por línea. NULL = `null`; Bool = `true`/`false`; Int64 y Double como números (los no finitos como `null`); Decimal como número si es un literal JSON válido y, si no, como cadena; el resto como cadena; Bytes en base64; Json (tipo de la columna) se inserta tal cual si es JSON válido y, si no, como cadena. Nombres de columna repetidos: se escriben todos, con sufijo `_2`, `_3`… en los repetidos.
  - SQL `INSERT`: `INSERT INTO <tabla> (<columnas>) VALUES (…), (…);` con `rowsPerInsert` filas por sentencia; identificadores con `dialect->quoteIdentifier`, textos con `quoteLiteral`, NULL literal, Bool como `TRUE`/`FALSE`, números tal cual, Bytes como `X'hex'` (sin dialecto: ANSI).
  - Markdown: tabla con cabecera, NULL vacío, `|` escapado como `\|` y saltos de línea como `<br>`.
- **Progreso y cancelación:** `progress(rows)` como mucho cada ~100 ms; `cancel()` llama a `Session::cancel()` y termina con `cancelled()`.
- **Seguridad (app):** el streaming solo admite sentencias que devuelven filas (`SELECT`, `WITH`, `TABLE`, `SHOW`, `EXPLAIN`) y pasa por `SqlSafety`. Lo valida la app antes de crear el `Exporter`.
- **Detalles fijados en la implementación:** `start()` no es un slot (moc no pasa un `unique_ptr`), pero sí `startBatches()` y `cancel()`. Bytes en CSV, TSV y Markdown también van en base64. NaN e Infinity van como texto en CSV, TSV y Markdown, como `NULL` en SQL y como `null` en JSON. La columna Json y JSON Lines se compactan (sin espacios ni saltos). Nombres JSON repetidos: `a, a, a_2` pasa a `a, a_2, a_2_2`. Markdown sin cabecera deja la fila de cabecera en blanco, porque la tabla la exige. SQL: una sentencia por línea. Carpeta inexistente o destino que es una carpeta dan `Internal`; un destino existente no escribible da `Permission`. `cancelled()` y `failed()` salen cuando el worker confirma que borró el temporal.
- **Límites conocidos:** el literal hexadecimal es siempre `X'..'` (`ISqlDialect` no tiene gancho; SQL Server necesitará `0x..`, marcado con `// shortcut`). No se valida UTF-8. `tableName` se cita como un solo identificador, sin separar esquema y tabla. `quoteLiteral` y `quoteIdentifier` del dialecto se llaman desde el hilo worker y deben ser sin estado.
- **Medido (Release, 6 columnas, CSV de 680 MB):** 10 M de filas en 6,6 s (1,5 M filas/s), memoria 14,9 MB al 10 % y 14,8 MB al final, pico 15,6 MB. Cancelar un resultado de 50 M filas tarda menos de 10 s y no deja archivos.

## 15. Distribución (H10)

- **Fases (cada una con su CI y su aprobación):** 1) base común (`install()`, icono, versión única, diálogo About, despliegue mínimo, prueba de instalación y tamaños); 2) Windows (Inno Setup y MSIX); 3) macOS (`.app` universal, `.dmg`, notarización); 4) Linux (AppImage y Flatpak).
- **Base común:** `install()` de `quarry`, los plugins de providers (`plugins/quarry/`) y las bibliotecas nativas que cada plataforma necesita junto al ejecutable (ver §8 y los providers). La versión sale de `project(quarry VERSION …)` y se propaga a la app (About, `QCoreApplication::applicationVersion`) y a los instaladores. Diálogo About con la versión, la licencia de Quarry (Apache-2.0), el aviso LGPLv3 de Qt y las licencias de terceros (Qt, libpq, libmariadb, libssh2, SQLite, KSyntaxHighlighting, QtKeychain, unixODBC, Lucide); la lista sale de un archivo generado en la CI, no escrito a mano.
- **Despliegue mínimo:** `windeployqt`, `macdeployqt` y `linuxdeploy` con los módulos que usa la app (Core, Gui, Qml, Quick, QuickControls2 y plugins de plataforma, imágenes y `QtQuick.Dialogs`), `--no-translations`, sin `qtvirtualkeyboard` y sin plugins de imagen que no se usen. En Linux se resuelve `libQt6DBus` para `libqt6keychain.so`. Qt sigue enlazada dinámicamente (LGPLv3).
- **Prueba de instalación en la CI:** instalar en un directorio limpio (sin Qt en el PATH ni `LD_LIBRARY_PATH`) y ejecutar `quarry --smoke` desde ahí. `bench_package` mide el tamaño de cada artefacto contra PERFORMANCE.md (Windows ≤ 45 MB, macOS ≤ 60 MB, Linux ≤ 70 MB).
- **Publicación:** workflow aparte `release.yml`, solo manual (`workflow_dispatch`), que genera los artefactos y como mucho una release en borrador. Ningún tag, publicación ni firma sin petición expresa del usuario.
- **Firma y notarización:** sin credenciales por ahora. Los pipelines generan artefactos sin firmar y dejan los pasos de firma (Windows: `signtool` sobre ejecutables, instalador y MSIX; macOS: `codesign` + `notarytool` + `stapler`) escritos y desactivados hasta que existan secretos del repositorio. Nunca se guarda un certificado en el repo. Un MSIX sin firmar no se instala (ni para probar).
- **Flatpak:** paquete `.flatpak` local construido en la CI con la misma Qt de aqt que el resto de formatos; sin Flathub (su runtime no trae Qt 6.12).
- **macOS universal:** dos compilaciones (arm64 y x86_64, triplets `arm64-osx-quarry` y `x64-osx-quarry`) unidas con `lipo`.
- **Sin actualización automática** en H10. La prueba en máquinas virtuales de las versiones mínimas (Windows 10 1809, macOS 14.4, Ubuntu 24.04) la hace el usuario.
- **Detalles fijados en la fase 1:**
  - Disposición: `<prefijo>/bin/quarry[.exe]`, `bin/plugins/quarry/` (providers), `bin/mariadb-plugins/`, las bibliotecas nativas junto al ejecutable en Windows y `LICENSE`, `NOTICE` y `THIRD_PARTY_LICENSES.txt` en la raíz. Reglas en `cmake/QuarryInstall.cmake`.
  - Windows: `windeployqt` falla con cualquier DLL cuyo nombre empiece por `qt` (toma `qt6keychain.dll` de vcpkg por un módulo de Qt), así que se ejecuta sobre un ejecutable auxiliar sin ese import (`packaging/windows/deploy_probe.cpp`, destino `quarry_deploy_probe`, que hay que compilar antes de `cmake --install`). Las DLL de vcpkg (libpq, OpenSSL, libmariadb, libssh2, zlib…) se resuelven con `file(GET_RUNTIME_DEPENDENCIES)` desde `quarry.exe` y los providers. El runtime de Visual C++ va como DLL junto al ejecutable (no `vc_redist.x64.exe`); la UCRT es parte de Windows 10.
  - Poda: sin estilos de Quick Controls distintos de `Basic` (la app fija ese estilo), sin plugins de `iconengines`, `qmltooling`, `networkinformation`, `tls` ni `generic`, sin traducciones de Qt ni `opengl32sw`.
  - macOS y Linux instalan solo la disposición (`// shortcut`): los paquetes relocalizables (`.app` con `macdeployqt`, AppImage con `linuxdeploy`) llegan con sus fases.
  - `THIRD_PARTY_LICENSES.txt` se genera al configurar (`cmake/ThirdPartyLicenses.cmake`): aviso de Qt (LGPLv3, enlace dinámico), licencia de Lucide y el `copyright` de cada puerto de vcpkg instalado. La app lo incrusta como `:/licenses/THIRD_PARTY_LICENSES.txt`.
  - Versión única en `project(quarry VERSION …)`: define `QUARRY_VERSION` para la app y rellena el recurso de versión de `quarry.exe` (`src/app/quarry.rc.in`, con el icono). Icono provisional generado por `scripts/make_app_icon.py` (Pillow) en `src/app/assets/app/`: `.ico`, `.icns` y PNG de 256 y 512; se sustituirá por uno diseñado.
  - Medido en Windows (Release, 2026-11-18): árbol instalado 70 MB (245 archivos), 28 MB comprimido con deflate (el presupuesto de instalador es 45 MB); `quarry --smoke` desde el árbol instalado con `PATH` mínimo sale con 0.
- **Fase 2, Windows (decisiones 2026-11-19):**
  - **Inno Setup** (`packaging/windows/quarry.iss`): `Quarry-<versión>-x64-setup.exe`, LZMA2 sólida, solo x64, mínimo Windows 10 1809 (17763), versión de CMake, `AppId` fijo, página de licencia, acceso directo en el menú Inicio y opcional en el escritorio. Instalación **por usuario por defecto** (`%LOCALAPPDATA%\Programs\Quarry`, sin administrador) con opción de instalar para todos los usuarios en Archivos de programa (entonces pide permisos). Inglés y español. La desinstalación **no borra** los datos del usuario (`%APPDATA%\Quarry`, llavero).
  - **MSIX** (`packaging/windows/AppxManifest.xml.in`): aplicación de escritorio con `runFullTrust`, mínimo 10.0.17763, logotipos generados desde el icono, empaquetado con `makeappx`. En MSIX las escrituras en `%APPDATA%` se redirigen a una carpeta por paquete: `quarry.db` y `known_hosts` quedan separados de los de una instalación Inno.
  - **Firma de prueba del MSIX:** la CI genera un certificado autofirmado temporal en cada ejecución, firma el MSIX con él y sube solo el `.cer` público; la clave privada no sale del job. Es solo para poder instalar y probar el MSIX (hay que confiar en el `.cer` a mano); no es una firma real. La firma real (`signtool` con un certificado propio) sigue desactivada hasta que haya secretos.
  - **CI:** solo en el nivel completo de Windows. Construye los dos paquetes, mide su tamaño contra el presupuesto (instalador ≤ 45 MB), los sube como artefactos de 3 días, e instala el de Inno en silencio en el runner, ejecuta `quarry --smoke` desde la carpeta instalada con un `PATH` mínimo y lo desinstala comprobando que no queda nada. El MSIX se valida (manifiesto y contenido con `makeappx`); ejecutarlo en el runner es complejo y la prueba de instalación real es del usuario.
  - **Implementado (fase 2):** `packaging/windows/build_installers.ps1` construye los dos paquetes desde el árbol que deja `cmake --install` (opciones `-TestSign`, `-SkipInno`, `-SkipMsix`, `-BudgetMB`; falla si el instalador supera el presupuesto). Inno Setup 6.3 o posterior (`x64compatible`). El MSIX usa el sujeto `CN=Quarry Test` como `Publisher`, que debe coincidir con el del certificado de firma; con una firma real habrá que cambiarlo. Los logotipos del manifiesto salen de `scripts/make_app_icon.py` (`packaging/windows/msix-assets/`). El script desempaqueta el MSIX recién creado como comprobación: un manifiesto mal formado falla ahí. Nada de esto se publica: son artefactos de la CI de 3 días.
  - **Medido (Windows, Release, 2026-11-19):** instalador 20,2 MB (presupuesto 45 MB) y MSIX 28,8 MB. Instalación por usuario en silencio, `quarry --smoke` desde la carpeta instalada con `PATH` mínimo (exit 0) y desinstalación sin restos (carpeta, entrada de desinstalación y menú Inicio; `%APPDATA%\Quarry` intacto).
  - **Límites:** el instalador no está firmado, así que SmartScreen avisará. El MSIX no se ha instalado nunca: instalarlo exige confiar en `Quarry-test.cer` como administrador (almacén "Personas de confianza"). Ni la instalación para todos los usuarios ni el idioma español del asistente se han recorrido a mano.
- **Fase 3, macOS (decisiones 2026-11-19):**
  - **Bundle:** `quarry` pasa a `MACOSX_BUNDLE` con `Info.plist` (`dev.quarry.Quarry`, versión de CMake, icono `.icns`, mínimo macOS 14.4, alta resolución): `Contents/MacOS/quarry`, `Contents/PlugIns/quarry` (providers, que `ProviderRegistry` ya busca ahí) y `Contents/Frameworks` (Qt y las dylib de vcpkg), desplegado con `macdeployqt`. Los plugins de autenticación de MariaDB se buscan en `Contents/PlugIns/mariadb` cuando se ejecuta desde un bundle (hoy, `<ejecutable>/mariadb-plugins`, que dentro de `Contents/MacOS` complicaría la firma).
  - **Universal:** dos compilaciones (arm64 con `arm64-osx-quarry` y x86_64 con `x64-osx-quarry`, preset nuevo) y un paso que une cada binario Mach-O con `lipo -create`. Qt de aqt ya es universal; de vcpkg solo `qtkeychain`, `KF6SyntaxHighlighting` y `unixodbc` son dinámicas, el resto va estático.
  - **CI en paralelo:** un job arm64 en `macos-15` y otro x86_64 en `macos-15-intel` (runner nativo, con sus tests), y un tercer job que une los dos, monta el `.app` y el `.dmg` y los prueba. Solo en el nivel completo. Depende de que GitHub siga ofreciendo el runner Intel (anunciado hasta agosto de 2027).
  - **`.dmg`:** `hdiutil` con el `.app` y un enlace a Aplicaciones, sin herramientas nuevas ni fondo decorado.
  - **Firma y notarización:** sin credenciales. Firma ad-hoc (`codesign -s -`), imprescindible para arrancar en Apple Silicon. Los pasos con Developer ID, runtime endurecido y `notarytool` + `stapler` quedan escritos y desactivados hasta que haya secretos. Un `.dmg` sin notarizar lo bloquea Gatekeeper: se abre con clic derecho → Abrir.
  - **Pruebas (CI):** `lipo -archs` (x86_64 y arm64) en cada Mach-O, montar el `.dmg`, copiar el `.app` y ejecutar `quarry --smoke` sin variables `DYLD_*`, medir contra el presupuesto de 60 MB y subir artefactos de 3 días. Nada se puede probar en la máquina Windows de desarrollo: todo se ve en la CI.
  - **Implementado (fase 3, sin verificar en un Mac):** `packaging/macos/` con `Info.plist.in`, `make_app.sh` (ensambla `Quarry.app` desde el **árbol de compilación**, no desde el de `cmake --install`, que pierde los RPATH sin los que `macdeployqt` no resuelve `libqt6keychain`, `libKF6SyntaxHighlighting` ni `libodbc`; ejecuta `macdeployqt` con `-qmldir`, `-libpath` y cada provider como `-executable`, **quita los RPATH absolutos** del build para que un bundle roto no arranque por casualidad en la máquina que lo construyó, y firma ad hoc), `make_universal.sh` (`lipo -create` de cada Mach-O fino; deja los de Qt, que ya son universales), `make_dmg.sh` (`hdiutil`, UDZO, con enlace a Aplicaciones) y `sign_and_notarize.sh` con `entitlements.plist` (Developer ID, runtime endurecido, `notarytool` y `stapler`; escrito pero **nunca ejecutado** ni enlazado en la CI). El bundle no sale del árbol de compilación: `quarry` sigue siendo un ejecutable simple y el `.app` se ensambla a partir del árbol de compilación. Cada `.app` por arquitectura se adelgaza (el Qt de aqt es universal y cada framework lleva las dos arquitecturas) y se poda: sin estilos de Quick Controls distintos de Basic, sin `sqldrivers`, `qmltooling`, `tls`, `networkinformation`, `iconengines` ni `generic`. El primer intento, sin podar ni adelgazar, dio 190 MB. El `.dmg` usa LZMA (`ULMO`).
  - **Presets** `macos-x64-debug` y `macos-x64-release` (triplete `x64-osx-quarry`, `CMAKE_OSX_ARCHITECTURES=x86_64`). `ci-plan.sh` da dos entradas de macOS en la matriz completa: `macOS (arm64)` en `macos-15` y `macOS (x64)` en `macos-15-intel`. Las cachés de vcpkg y de ccache llevan la arquitectura en la clave.
  - **CI (nivel completo):** cada job de macOS construye su `.app`, lo prueba con `env -i` (sin `DYLD_*`), comprueba que el árbol instalado trae el provider de SQLite y avisa si no ve los providers; sube un tar. `macos-universal` (en `macos-15`) une los dos, exige que cada Mach-O tenga las dos arquitecturas, hace el `.dmg`, comprueba el presupuesto de 60 MB, lo monta, copia el `.app` y lo arranca (y su parte x86_64 con Rosetta si el runner la tiene). `macos-intel-check` arranca ese mismo `.dmg` en el runner Intel. El plugin `offscreen` de Qt solo se copia en las pruebas y no entra en el paquete.
  - **Rutas:** los plugins de autenticación de MariaDB se buscan en `Contents/PlugIns/mariadb` cuando el ejecutable está en `Contents/MacOS` (`mysql::pluginDirectory`, con test).
  - **Límites:** nada de esto se ha podido ejecutar en la máquina de desarrollo (Windows): todo se ve por primera vez en la CI de macOS, con riesgo de varias iteraciones. Depende del runner `macos-15-intel`. El `.dmg` está firmado ad hoc, no notarizado: Gatekeeper lo bloquea y se abre con clic derecho → Abrir (o `xattr -dr com.apple.quarantine Quarry.app`).
- **Fase 4, Linux (decisiones 2026-11-19):**
  - **AppImage** (job de Linux del nivel completo, que ya tiene la compilación): `cmake --install --prefix AppDir/usr` (la disposición actual, `usr/bin/quarry`, `usr/bin/plugins/quarry` y `usr/bin/mariadb-plugins`, ya es la que pide el AppDir), `linuxdeploy` con su plugin de Qt (el QML de la app decide los módulos) y poda como en Windows y macOS (sin estilos de Quick Controls distintos de Basic, sin drivers SQL ni plugins de depuración). `AppRun` fija `ODBCSYSINI=/etc` si no está definida, para que unixODBC encuentre los drivers del sistema. Entrada `.desktop`, metadatos AppStream (Apache-2.0) e icono en `packaging/linux/`. Salida `Quarry-<versión>-x86_64.AppImage`, presupuesto ≤ 70 MB. Se construye en Ubuntu 24.04 y exige una glibc igual o más reciente que la suya.
  - **Herramientas:** `linuxdeploy`, su plugin de Qt y `appimagetool`, bajadas en la CI **fijadas por versión y con comprobación de SHA-256** (un cambio en la descarga rompe la CI). Solo se usan en la CI: no entran en Quarry ni en el paquete.
  - **Flatpak local** (job aparte que consume el AppDir ya desplegado): `packaging/linux/flatpak/dev.quarry.Quarry.yml` sobre el runtime `org.freedesktop.Platform`, con un módulo que copia el AppDir (el runtime no trae Qt 6.12; sin Flathub). Salida `Quarry-<versión>.flatpak`. Permisos: red, X11 y Wayland, DRI, el agente SSH (`--socket=ssh-auth`), el Secret Service (`--talk-name=org.freedesktop.secrets`) y **solo la carpeta personal** (`--filesystem=home`): el portal de archivos no basta porque SQLite crea archivos junto al elegido (`-wal`, `-journal`). Los archivos en otros discos exigen un permiso manual (`flatpak override`).
  - **Pruebas (CI):** el AppImage se ejecuta con `--appimage-extract-and-run`, en entorno mínimo y sin `LD_LIBRARY_PATH`, y no debe quedar ninguna biblioteca sin resolver (`ldd`); el Flatpak se instala en el usuario del runner y se ejecuta con `quarry --smoke` (hay que relajar `kernel.apparmor_restrict_unprivileged_userns`). Nada se puede probar en la máquina Windows de desarrollo.
  - **Implementado (fase 4):** `packaging/linux/` con `build_appimage.sh` (descarga `linuxdeploy` 1-alpha-20251107-1, su plugin de Qt 1-alpha-20250213-1 y `appimagetool` 1.9.1, comprueba su SHA-256 y construye el AppDir con `cmake --install`, `linuxdeploy --deploy-deps-only` sobre los providers, la poda y `appimagetool`), `AppRun` propio, `test_appimage.sh`, `dev.quarry.Quarry.desktop`, `dev.quarry.Quarry.metainfo.xml.in` (el script rellena versión y fecha), `build_flatpak.sh` y `flatpak/dev.quarry.Quarry.yml` (runtime `org.freedesktop.Platform` 25.08).
  - **CI (nivel completo):** el job de Linux construye y prueba el AppImage, comprueba el presupuesto de 70 MB **después** de subir los artefactos (para poder descargar uno que se pase y medirlo) y sube el AppDir desplegado; el job `linux-flatpak` lo toma, instala `flatpak` y el runtime desde Flathub, construye el `.flatpak`, lo instala y arranca Quarry desde él. El plugin `offscreen` de Qt solo se copia en las pruebas.
  - **Prueba del AppImage:** se desempaqueta (sin FUSE) y, con un entorno vacío, cada ELF debe resolver todas sus bibliotecas (`ldd` sin `not found`) y `quarry --smoke` sale con 0 y ve los providers.
  - **Límites:** nada de esto se ha podido ejecutar en la máquina de desarrollo (Windows): todo se ve por primera vez en la CI, con riesgo de varias iteraciones. Riesgos conocidos: el tamaño del AppImage (ICU del Qt de aqt); que `linuxdeploy-plugin-qt` (de febrero de 2025) no entienda Qt 6.12; que el runner no permita los espacios de usuario del sandbox; el llavero (`libsecret`, D-Bus); y que el runtime 25.08 no esté disponible. El Flatpak se instala desde un archivo `.flatpak` y exige que el runtime esté instalado desde Flathub.
  - **Resultado (fase 4, CI 2026-11-19):** AppImage 54 MB (presupuesto 70 MB) y Flatpak 39 MB; ambos arrancan con los 5 providers en el runner. Ajustes sobre lo previsto: se poda también la biblioteca del estilo nativo de Quick Controls (enlaza el de Fusion) y el job del Flatpak instala `elfutils` (`eu-strip`). No se han probado en un Linux real (pantalla, llavero, sandbox).
