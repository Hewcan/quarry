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
| Telemetría | Ninguna. Sin conexiones salientes salvo las que el usuario abre hacia sus bases de datos |
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
| libssh2 | vcpkg | túneles SSH (hito posterior) |

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
` y ``, y la barra como `\`.
  - JSON: un array de objetos (`[` y `]` en streaming, un objeto por línea); JSON Lines: un objeto por línea. NULL = `null`; Bool = `true`/`false`; Int64 y Double como números (los no finitos como `null`); Decimal como número si es un literal JSON válido y, si no, como cadena; el resto como cadena; Bytes en base64; Json (tipo de la columna) se inserta tal cual si es JSON válido y, si no, como cadena. Nombres de columna repetidos: se escriben todos, con sufijo `_2`, `_3`… en los repetidos.
  - SQL `INSERT`: `INSERT INTO <tabla> (<columnas>) VALUES (…), (…);` con `rowsPerInsert` filas por sentencia; identificadores con `dialect->quoteIdentifier`, textos con `quoteLiteral`, NULL literal, Bool como `TRUE`/`FALSE`, números tal cual, Bytes como `X'hex'` (sin dialecto: ANSI).
  - Markdown: tabla con cabecera, NULL vacío, `|` escapado como `\|` y saltos de línea como `<br>`.
- **Progreso y cancelación:** `progress(rows)` como mucho cada ~100 ms; `cancel()` llama a `Session::cancel()` y termina con `cancelled()`.
- **Seguridad (app):** el streaming solo admite sentencias que devuelven filas (`SELECT`, `WITH`, `TABLE`, `SHOW`, `EXPLAIN`) y pasa por `SqlSafety`. Lo valida la app antes de crear el `Exporter`.
- **Detalles fijados en la implementación:** `start()` no es un slot (moc no pasa un `unique_ptr`), pero sí `startBatches()` y `cancel()`. Bytes en CSV, TSV y Markdown también van en base64. NaN e Infinity van como texto en CSV, TSV y Markdown, como `NULL` en SQL y como `null` en JSON. La columna Json y JSON Lines se compactan (sin espacios ni saltos). Nombres JSON repetidos: `a, a, a_2` pasa a `a, a_2, a_2_2`. Markdown sin cabecera deja la fila de cabecera en blanco, porque la tabla la exige. SQL: una sentencia por línea. Carpeta inexistente o destino que es una carpeta dan `Internal`; un destino existente no escribible da `Permission`. `cancelled()` y `failed()` salen cuando el worker confirma que borró el temporal.
- **Límites conocidos:** el literal hexadecimal es siempre `X'..'` (`ISqlDialect` no tiene gancho; SQL Server necesitará `0x..`, marcado con `// shortcut`). No se valida UTF-8. `tableName` se cita como un solo identificador, sin separar esquema y tabla. `quoteLiteral` y `quoteIdentifier` del dialecto se llaman desde el hilo worker y deben ser sin estado.
- **Medido (Release, 6 columnas, CSV de 680 MB):** 10 M de filas en 6,6 s (1,5 M filas/s), memoria 14,9 MB al 10 % y 14,8 MB al final, pico 15,6 MB. Cancelar un resultado de 50 M filas tarda menos de 10 s y no deja archivos.
