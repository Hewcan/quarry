# Roadmap de Quarry

Hitos pequeños y verificables. Al terminar cada uno: compilar en los tres sistemas (CI), pasar los tests, comprobar los criterios de aceptación y los presupuestos de [PERFORMANCE.md](PERFORMANCE.md), actualizar este archivo y **esperar el visto bueno** antes del siguiente.

Estados: `pendiente` · `en curso` · `hecho` · `bloqueado`

| Hito | Contenido | Estado |
|---|---|---|
| F0 | Entorno de desarrollo (Windows verificado) | hecho |
| H0 | Esqueleto + CI en los tres sistemas | hecho (2026-11-12) |
| H1 | Versión mínima (subhitos 1.1–1.8) | hecho (2026-11-15; pendientes las pasadas manuales con lectores de pantalla de 1.5–1.7) |
| H2 | Edición de datos | hecho (2026-11-15; pendiente la pasada manual con lectores de pantalla) |
| H3 | Autocompletado | hecho |
| H4 | Historial y consultas guardadas | hecho |
| H5 | Exportación | pendiente |
| H6 | MySQL / MariaDB | pendiente |
| H7 | SQL Server + ODBC genérico | pendiente |
| H8 | Túneles SSH | pendiente |
| H9 | EXPLAIN visual | pendiente |
| H10 | Distribución | pendiente |
| — | Oracle (ODPI-C) | sin fecha |

---

## H0 — Esqueleto del proyecto

**Objetivo:** estructura `core` / `providers` / `app` que compila y pasa los tests en la CI de Windows, macOS y Linux, reutilizando los scripts de la FASE 0.

- Renombrar el proyecto a `quarry`: carpeta, `project()`, venv de aqt, docs. Añadir `LICENSE` (Apache-2.0) y `NOTICE`.
- `vcpkg.json` con `builtin-baseline` y las dependencias de H1 (`sqlite3`, `libpq`, `qtkeychain-qt6`, `syntax-highlighting`), más `vcpkg-overlays/` (shim de `qtbase`/`qttools` + triplets `*-quarry`) según ARCHITECTURE §9.
- Targets: `quarry_provider_api` (INTERFACE), `quarry_core` (Qt6::Core), `quarry_provider_dummy` (MODULE de prueba), `quarry` (exe con una ventana vacía y el estilo Basic propio).
- `cmake/QuarryHelpers.cmake`: `quarry_add_provider()`, `quarry_add_test()`, warnings estrictos (`/W4 /permissive-` · `-Wall -Wextra -Wpedantic`), warnings como error en la CI.
- `.clang-format`, `.clang-tidy` (subconjunto razonable) y `.editorconfig`.
- GitHub Actions `ci.yml`: matriz `windows-2022`, `macos-15` (arm64; `macos-14` ya retirada), `ubuntu-24.04`. Pasos: `scripts/setup.* --non-interactive` → `cmake --preset <os>-release` → build → `ctest`. Caché de aqt y de vcpkg (binary caching con `actions/cache`).
- Repo privado en GitHub (`gh repo create`, **con confirmación antes del push**).

**Criterios de aceptación** (CI en verde: run 43319872951, commit `1545348`)
- [x] `ctest --preset <os>-debug` pasa en local (Windows, 3/3 con warnings como errores) y la CI está en verde en Windows (`windows-2022`), macOS (`macos-15`, arm64) y Linux (`ubuntu-24.04`, con clang-tidy), más el job de clang-format.
- [x] Test: `ProviderRegistry` lista el plugin dummy por metadatos sin cargarlo y lo carga al pedir `provider("dummy")` (`tst_provider_registry`, que además rechaza un plugin con API `/2`).
- [x] Test de arquitectura: `quarry_assert_no_ui_deps()` hace fallar la configuración si `quarry_core` enlaza Gui/Qml/Quick/Widgets.
- [x] `quarry --smoke` sale con 0 y sin warnings de QML en los tres sistemas (offscreen en la CI; nativo en Windows). La detección de warnings se comprobó inyectando uno: sale con 2.
- [x] QtKeychain y KSyntaxHighlighting compilan con vcpkg contra Qt 6.12 de aqt en los tres sistemas. En `vcpkg_installed` solo está el shim vacío de `qtbase`.

**Desviaciones**
- La carpeta local sigue llamándose `dbmanager`: no se pudo renombrar porque la sesión de trabajo la tenía abierta. Todo lo demás ya usa `quarry`.
- El repo `Kilamper/quarry` es **público** desde el 2026-11-12 (estaba previsto como privado). La historia se reescribió con el email noreply antes de publicarlo. Desde el 2026-11-15 vive en la organización **`Hewcan/quarry`** (transferido; la URL antigua redirige).
- Ejecutables en la raíz del build en todas las plataformas: en macOS `src/app/quarry` chocaba con el directorio `src/app/Quarry` del módulo QML (`errno=21`).
- CI: `macos-14` ya está retirada (se usa `macos-15`); `setup.sh` instala autotools (libgpg-error ← libsecret ← QtKeychain).
- Triplet de Windows: se antepone `$QT_ROOT_DIR/bin` al PATH en los ports que usan Qt, porque `katehighlightingindexer` fallaba con `0xC0000135` (falta de DLL).

---

## H1 — Versión mínima

Se divide en subhitos y cada uno se puede revisar por separado. Al final de H1, el usuario puede conectarse a SQLite y PostgreSQL, explorar el esquema, escribir SQL con resaltado, ejecutarlo de forma asíncrona y cancelable, y ver millones de filas sin que la UI se bloquee.

### H1.1 — Núcleo de ejecución + provider SQLite · **hecho (2026-11-13)**
- `Result<T>`, `Error`, `CancellationToken`, `ResultBatch`/`ColumnBuffer`, interfaces de provider (ARCHITECTURE §3–4).
- `SessionWorker` / `SessionManager` (§5), señales `columnsReady`/`batchReady`/`finished`/`failed`.
- Provider SQLite: abrir archivo y `:memory:`, `children()` (tablas, vistas, columnas, índices, FKs), streaming, `sqlite3_interrupt`, dialecto con `splitStatements`.
- Suite de conformidad de providers (`tests/providers/conformance/`).

Criterios:
- [x] Conformidad SQLite en verde (`tst_sqlite_provider`: 18 casos de conformidad + 17 específicos): tipos, NULLs, 0 filas, columnas mixtas, parámetros, errores con `position` (también tras caracteres no ASCII), `splitStatements` (comentarios, comillas, triggers). `execute` rechaza varias sentencias con `Unsupported`.
- [x] Cancelar un `WITH RECURSIVE` infinito devuelve `Cancelled` en < 200 ms y la conexión sigue usable. Una cancelación tardía con la sesión inactiva no aborta la siguiente sentencia.
- [x] `SELECT` de 1 M de filas en batches de 500 a través de `Session` y el plugin real: el hilo principal sigue procesando eventos (hueco máximo de 5 ms). Tarda 289 ms en Release.
- [x] Bench (`bench_core`, Release, máquina de referencia): **442 ms** (2,26 M filas/s) y **17 MB** de pico de RSS, frente a ≤ 1,5 s y ≤ 200 MB. Los batches se descartan al leerlos; la RAM con 1 M filas cargadas se mide en H1.7.
- Sin probar: SQLite con la base bloqueada por otra conexión (la traducción a `Timeout`) ni conexiones concurrentes. KSyntaxHighlighting no tiene definición específica de SQLite; se usa "SQL".

### H1.2 — Shell de la aplicación, tema y componentes base · **hecho (2026-11-13)**
- Ventana principal: barra lateral, área de pestañas, panel de resultados y barra de estado (UX.md §3).
- `ThemeManager`: claro, oscuro y alto contraste, siguiendo `QStyleHints::colorScheme` y `accessibility()->contrastPreference/motionPreference`, con posibilidad de forzarlos desde los ajustes.
- Componentes del estilo propio (`Quarry.Controls`): `QButton`, `IconButton`, `QTextField`, `QComboBox`, `QCheckBox`, `QDialog`, `QMenu`, `QToolTip`, `QSplitView`, `QTabBar`, `FocusRing`, `Heading`, con foco visible y `Accessible.*`.
- Paleta de comandos (Ctrl/Cmd+K) con `CommandRegistry`.
- i18n: `qt_add_translations`, `quarry_es.ts`.

Criterios:
- [x] Test QML (`tests/app/A11y.qml`, usado en `tst_controls` y `tst_mainwindow`): todo `Item` enfocable tiene `Accessible.name` y un rol distinto de `NoRole`, en la ventana completa y también con la paleta y los ajustes abiertos.
- [x] Test de contraste (`tst_theme_contrast`): texto ≥ 4.5:1 (el mínimo medido es 5.22) e indicadores ≥ 3:1 (mínimo 4.19), en los cuatro temas fijos.
- [x] El tema cambia en caliente con el sistema (esquema de color, alto contraste, movimiento reducido) y se puede forzar en Ajustes. El alto contraste real de Windows solo se ha probado con la paleta simulada en tests.
- [x] Toda la ventana se recorre con Tab, Shift+Tab y F6/Shift+F6, más Mod+1/2/3, sin trampas de foco. Los diálogos atrapan el foco y lo devuelven al cerrarse.
- [x] Arranque en caliente: mediana de **366 ms** (`--bench-startup`, Release, 7 ejecuciones) frente a ≤ 400 ms. Una ventana vacía de Qt cuesta ~320–335 ms en esta máquina, así que el margen es pequeño (ver PERFORMANCE.md).
- Además: paleta de comandos (Mod+K / Mod+Shift+P), Ajustes (Mod+,), comandos para mostrar y ocultar paneles y restablecer el diseño, interfaz en español (79 textos) con cambio de idioma en caliente.
- Sin probar: la pasada manual con lectores de pantalla (`docs/a11y-checklist.md`) y si se anuncia bien el elemento activo de la paleta.

### H1.3 — Conexiones y credenciales · **hecho (2026-11-13)**
- `AppDatabase` (`quarry.db`, migraciones) y `ConnectionStore`.
- Diálogo de conexión generado a partir de `connectionFields()`, con la sección avanzada plegada y el botón "Test connection".
- `CredentialStore` (QtKeychain) con "Save password" o "Ask every time".
- Color por conexión y marca de producción (rojo + icono + texto "PRODUCTION").
- Estado vacío: "No connections yet: create one or open a SQLite file", con botones.

Criterios:
- [x] Tras guardar una conexión, `quarry.db` no contiene la contraseña (`tst_connection_store::secretsAreNeverWritten` busca la cadena en el archivo; `tst_connection_controller` comprueba que los campos secretos se eliminan antes de guardar).
- [x] La contraseña se escribe y se lee del llavero real en Windows (Credential Manager, `QUARRY_TEST_KEYCHAIN=1`) y en Linux (CI, `gnome-keyring` headless). macOS: pendiente de la prueba manual.
- [x] Un fallo de conexión muestra un mensaje claro, el detalle técnico plegado y lo anuncia al lector de pantalla (`tst_connections.qml`).
- Además: lista de conexiones con estado vacío, diálogo generado desde `connectionFields()` (sección avanzada plegada, "Test connection" cancelable), color por conexión, marca PRODUCTION (texto + icono) que exige escribir el nombre para borrar, duplicar, abrir archivo SQLite (Mod+O), todo con teclado y recorrido por el walker de accesibilidad. 160 textos en español.
- Arranque: el escaneo de providers y el hilo del store salen del camino de arranque (después del primer frame). La mediana medida fue 786 ms con la máquina cargada por otro proceso; el árbol anterior medía lo mismo con la misma carga (768 vs 770 ms). **Pendiente** repetir `--bench-startup` en una máquina tranquila (objetivo ≤ 400 ms).
- Sin probar: la pasada manual con lectores de pantalla del diálogo de conexión; compilación local en Linux/macOS (solo CI). El área desplazable del diálogo no tiene barra de desplazamiento visible.

### H1.4 — Árbol de esquemas bajo demanda · **hecho (2026-11-14)**
- `SchemaTreeModel` (QAbstractItemModel) con `canFetchMore`/`fetchMore` por nodo y una sesión de metadatos propia.
- Nodos: conexión → base de datos → esquema → carpetas (Tables, Views, …) → objeto → columnas/índices/FKs.
- Teclado: flechas, ←/→ para plegar y desplegar, Enter para abrir, búsqueda escribiendo, menú contextual (Shift+F10 / tecla Menú).

Criterios:
- [x] `QAbstractItemModelTester` (modo Fatal) sin errores en `tst_schema_tree_model`: raíces que se añaden, renombran, mueven y borran mientras cargan, y carga → error → reintento → desconexión.
- [x] 10 000 tablas: el modelo las despliega en 50 ms (Release) con un hueco máximo del event loop de 11 ms. A través del `TreeView` real, la mediana de tres despliegues en frío es 22–25 ms (máximo 31 ms) aun con la suite en paralelo, frente a 50 ms. La fila "Loading…" se anuncia. El test usa la mediana porque Qt Quick tiene picos sueltos de 24–36 ms que dependen de la carga; trocear las inserciones no los reducía. En los runners de macOS de la CI el event loop ya se para ~90 ms sin hacer nada, así que allí el test se salta con un mensaje; el presupuesto se comprueba en Windows y Linux.
- [x] Cada fila expone a la accesibilidad el rol `TreeItem`, el nombre y el estado expandido de forma nativa (comprobado con `QAccessibleInterface` en `test_accessibilityInterfaceOfTheRows`). Nivel y posición van como texto en `Accessible.description` ("level 3, 1 of 2"), porque Qt no tiene propiedades para ellos. **Pendiente** de la pasada manual con NVDA/VoiceOver.
- Además: un solo árbol con las conexiones como raíz (color, PRODUCTION y menú se conservan); conectar al desplegar, con contraseña del llavero o pedida (`PasswordDialog`, solo en memoria); comando `connection.disconnect`; filas de error con Retry y detalle, y "(empty)"; búsqueda escribiendo; iconos por tipo de objeto; 37 textos nuevos en español.
- Sin probar: el diálogo de contraseña de principio a fin desde QML (SQLite no tiene campo secreto; está cubierto en C++); motores con esquemas (llega con PostgreSQL en H1.8).
- Limitaciones conocidas: Qt no da nombre accesible a los campos de contraseña (se usan una etiqueta visible y `Accessible.description`; también afecta al diálogo de conexión); el árbol no tiene barra de desplazamiento visible (Quarry.Controls aún no tiene `QScrollBar`); los cambios al editar una conexión conectada se aplican al reconectar.

### H1.5 — Editor SQL · **hecho (2026-11-14)**
- Editor sobre `TextEdit` con KSyntaxHighlighting (`QQuickTextDocument`) y la definición del dialecto.
- Números de línea, sentencia actual resaltada, Ctrl/Cmd+Enter (sentencia actual) y Ctrl/Cmd+Shift+Enter (todo).
- Pestañas de editor (Ctrl/Cmd+T, Ctrl/Cmd+W) asociadas a una conexión.

Criterios:
- [x] Resaltado correcto para SQLite y PostgreSQL (`$$…$$`, comentarios anidados), comprobado en `tst_sql_document` con las definiciones "SQL" y "SQL (PostgreSQL)". Los delimitadores `$$` van como operador y el cuerpo como una sola cadena; `/* /* */ */` es un comentario, también en varias líneas.
- [x] Teclear en 10 000 líneas: `bench_editor` (Release, plataforma nativa de Windows, de la tecla al texto resaltado y maquetado) da p50 4.9–5.9 ms y p95 10.7–12.1 ms. Con la CPU al 83 % por otra aplicación, la misma medida sube a p95 20–22 ms. En ctest, como `offscreen` infla las cifras, `tst_editor` compara 10 000 líneas con 200 y exige que el p95 no suba más de 16 ms. El coste crece con los bloques que hay detrás de la edición (QTextDocumentLayout).
- [x] El editor expone rol `EditableText` con nombre ("SQL editor, Query 1") y descripción (conexión, producción, cómo salir con el teclado); la línea y la columna están en la barra de estado ("Ln 12, Col 3"); el recorrido de accesibilidad pasa sobre el editor, el selector de conexión y el diálogo de cierre. **Pendiente** de la pasada manual con NVDA/VoiceOver.
- Además: pestañas guardadas en `quarry.db` (migración 2) y restauradas tras el primer frame; Mod+T con la conexión del nodo del árbol; selector de conexión en la cabecera; Mod+W con confirmación si hay texto; Ctrl+Tab / Ctrl+Shift+Tab; banda de la sentencia actual y marcado de Mod+Enter / Mod+Shift+Enter con anuncio (la ejecución llega en H1.6); números de línea virtualizados (~40 items para 10 000 líneas); textos en español. Arranque: +6 ms (393 → 399 ms, mediana de 7, Release) por la DLL de KSyntaxHighlighting.
- Limitaciones conocidas: sin sangría de varias líneas; Mod+Shift+Enter aún no ejecuta solo la selección; sin barra de desplazamiento visible; la banda de la sentencia actual puede ir hasta 250 ms por detrás de una edición; los números de línea suponen líneas de altura uniforme.

### H1.6 — Ejecución asíncrona y cancelable en la UI — hecho (2026-11-14)
- Barra de estado: `Running… 00:03`, botón Cancel (Esc / Ctrl/Cmd+.), filas y tiempo al acabar.
- Panel de mensajes: errores con `position` subrayada en el editor, notices y filas afectadas.
- `SqlSafety`: confirmación para `UPDATE`/`DELETE` sin `WHERE`, y para `DROP`/`TRUNCATE` en conexiones de producción.
- Decisiones (2026-11-14, ARCHITECTURE §6.1): lexer genérico en `core` para `SqlSafety`; ejecutar todo se para en el primer error; un solo diálogo de confirmación para todo el plan; Results muestra un resumen hasta que llegue el grid (H1.7).

Criterios:
- [x] La UI responde durante la consulta: con una CTE recursiva lenta de SQLite, `tst_query` cambia de pestaña, despliega y pliega el árbol y cancela con Mod+.; huecos del event loop (descontado el suelo en reposo de 24 ms) de 26–33 ms en Release (presupuesto 50 ms).
- [x] El inicio y el fin de la ejecución se anuncian (`Accessible.announce`) sin robar el foco; los errores, de forma enérgica, con línea y columna. Comprobado en `tst_query` (foco intacto) y en el recorrido de accesibilidad sobre Results, Messages y el diálogo de confirmación. **Pendiente** de la pasada manual con NVDA/VoiceOver.
- [x] Tests de `SqlSafety` (`tst_sql_safety`, ~50 casos): detecta `DELETE FROM t`, `delete from t -- where`, y no da falso positivo con `DELETE … WHERE` ni con CTE + `WHERE`. 10 000 sentencias en 6 ms (Release).
- Además: una `Session` por pestaña, creada en la primera ejecución y cerrada con la pestaña, al cambiar de conexión o al borrarla (`tst_query_controller`, 15 casos); un plan nuevo cancela el anterior; contraseña "Ask every time" con el diálogo de contraseña, y un fallo de autenticación olvida la recordada; flujo de confirmación solo con teclado; zoom al 200 % con la consulta en marcha; textos en español. Arranque sin cambio medible (las sesiones son perezosas): A/B 759–813 → 792–820 ms con la máquina cargada, que también infla la base; repetir en una máquina tranquila.
- Limitaciones conocidas: el plugin del provider se carga en el hilo de la UI en la primera ejecución de cada motor (como el árbol); no se guardan filas hasta H1.7; subrayado continuo, no ondulado (evita el módulo Qt Quick Shapes); "Ask every time" solo tiene test en C++; Mod+. sin probar en macOS/Linux; `EXPLAIN (ANALYZE false) …` pide confirmación (falso positivo).

### H1.7 — Grid de resultados virtualizado — hecho (2026-11-15), pendiente de la pasada manual con lectores de pantalla
- `ResultTableModel` sobre `ResultBatch` (texto solo en `data()`), `fetchMore` de 500 filas y "Fetch all".
- `QuarryGrid`: `TableView` con cabeceras fijas, selección por celda, rango o fila, copiar (TSV), ajuste de ancho de columna y navegación completa con teclado.
- **Spike de accesibilidad** (ARCHITECTURE §13): `QAccessibleTableInterface` propio. Si no es viable en el plazo del subhito, se pasa al plan B con `announce`, y la decisión se documenta.
- NULL se muestra como texto "NULL" en cursiva y atenuado (no solo con color); los BLOB como `<binary 2.3 KB>`.
- Decisiones (2026-11-14, ARCHITECTURE §6.2): `CellFormatter` genérico en `core`; con varios resultados el grid muestra el último con columnas; Mod+C copia TSV sin cabecera y Mod+Shift+C con cabecera.

Criterios:
- [x] Bench (`bench_grid`, Release, plataforma nativa de Windows, 1 M filas × 10 columnas de SQLite): primer batch visible 26–33 ms (frío 32–53 ms); "Fetch all" 506 ms; RAM 259 MB en total, 215 MB sobre el reposo (44 MB) gracias a `ColumnBuffer::shrinkToFit()` (antes 299 MB); scroll por todo el rango p95 14.95 ms, máximo 16.3 ms, 0 frames > 33 ms.
- [ ] NVDA, VoiceOver y Orca anuncian la cabecera de columna y el valor al moverse por las celdas: con el plan B (ARCHITECTURE §13) cada movimiento anuncia "Row X of N, <columna>: <valor>", comprobado en `tst_grid`. **Pendiente** de la pasada manual (lista en UX.md, "Grid de resultados").
- [x] `QAbstractItemModelTester` sin errores durante el streaming (`tst_result_table_model`).
- Además: copiar TSV con y sin cabecera (tope de 100 000 celdas con aviso); Fetch all con anuncios y "Stop fetching"; ancho de columna con ratón y con Alt+Shift+←/→; regla del último resultado; `QScrollBar` nuevo en `Quarry.Controls`; textos en español. Arranque sin coste medible del grid (carga perezosa; A/B 426–428 ms con la máquina cargada).
- Limitaciones conocidas: plan A (tabla accesible real) aparcado por depender de `Qt6::QuickPrivate`; sin el límite de memoria de Fetch all (2 M filas / 1 GB, §5); a partir de ~670 000 filas la posición de las filas pierde precisión (~2 px, límite de `float` en Qt); las cabeceras no son nodos accesibles propios; si el plan acaba con una sentencia sin columnas, el grid queda en "not all loaded" sin poder traer más; Enter en una tabla del árbol todavía no la abre.

### H1.8 — Provider PostgreSQL — hecho (2026-11-15)
- libpq 18: `PQsendQueryParams` + `PQsetChunkedRowsMode`, cancelación con `PQcancelCreate`/`PQcancelBlocking`, SSL (`sslmode`), introspección por `pg_catalog`.
- Tipos: int2/4/8, numeric (texto exacto), float, bool, text/varchar, bytea, date/time/timestamp(tz), uuid, json/jsonb, arrays (como texto en v1).
- CI: contenedor de servicio `postgres:17` en el job de Linux; la conformidad corre allí. En Windows y macOS se compila y solo corren los tests unitarios.
- Decisiones (2026-11-15, ARCHITECTURE §6.3): árbol con la base de datos del perfil sin esquemas del sistema; SSL con `sslmode` + `sslrootcert`; tests locales con los binarios en zip de PostgreSQL 17 en `%TEMP%`.

Criterios:
- [x] Conformidad PostgreSQL en verde en la CI (Linux, `postgres:17`, run 43727040618; `tst_postgres_provider`: 17 pruebas compartidas + 29 propias; en local, con PostgreSQL 17.12 portátil, 46/46; sin servidor, 12 pasan y 34 se saltan).
- [x] Cancelar `SELECT pg_sleep(60)` vuelve en 42–43 ms y la siguiente consulta funciona. `requestCancel()` ya no bloquea a quien la llama (64–92 µs; `Session::cancel()` 70 µs): la conexión de cancelación va en un hilo propio, y la llamada siguiente espera a que termine, así una cancelación tardía nunca alcanza a la sentencia siguiente (30 rondas en `lateCancelNeverHitsNextStatement`).
- [x] El error de sintaxis marca la posición correcta: `PG_DIAG_STATEMENT_POSITION` (caracteres, base 1) se convierte a offset UTF-16; con `SELECT 'é日本😀', 1 FROM FROM t` el servidor da 22 y el provider 23, justo en `FROM t` (en bytes sería 30). El editor ya subraya `Error::position` (H1.6).
- Además: árbol Database → Schema → Table/View/MaterializedView → Columns/Indexes/Foreign keys sin esquemas del sistema; formulario con `sslmode` y `sslrootcert`; tipos por OID (bytea decodificado de hex, numeric exacto, arrays como texto); notices; COPY y varias sentencias en una llamada → `Unsupported`; una consulta abandonada a medias se cancela y se drena (44 ms con 50 M filas pendientes); libpq y OpenSSL junto a `quarry.exe` (§8).
- Limitaciones conocidas: parámetros como texto con OID 0; sin `ColumnInfo::sourceTable` ni typmod en `nativeType`; `open()` no se puede cancelar (lo acota `connect_timeout` de 15 s, igual que la conexión de cancelación); los cuerpos `BEGIN ATOMIC` se parten en su `;` interior; sin prueba de `sslmode=require`/`verify-*` con éxito (los servidores de prueba no tienen SSL); "Technical details" solo con `PQresultErrorMessage`; no hay test de la app de punta a punta contra PostgreSQL.

---

## H2 — Edición de datos — hecho (2026-11-15)
- Edición en el grid de tablas con PK (o con ROWID/ctid y aviso), insertar y borrar filas.
- Los cambios se acumulan en local con indicador visual (icono + texto, no solo color). **Vista previa del SQL** generado antes de aplicar. Se aplica en una transacción.
- Deshacer/rehacer de cambios pendientes (Ctrl/Cmd+Z / Shift+Z).
- Conexiones de producción: confirmación reforzada (hay que escribir el nombre de la conexión).
- Decisiones (2026-11-15, ARCHITECTURE §6.4): solo se editan tablas abiertas desde el árbol (Enter / "Open data"); valores como parámetros, con la vista previa mostrando el SQL y los valores; `WHERE` solo por la clave y 1 fila exacta o rollback; `ISqlDialect` gana `placeholder()`, `qualifiedName()` y `rowIdentifier()`; celdas BLOB de solo lectura.

Criterios:
- [x] La vista previa coincide exactamente con lo que se ejecuta. `tst_data_editing` comprueba que `changesSent` es igual a `previewStatements()`: las dos salen de `ChangeSqlBuilder::build` sobre los mismos deltas.
- [x] Si falla a mitad, se hace rollback y los cambios siguen pendientes. Está probado con SQLite y con PostgreSQL 17 (`tst_data_editing`, 18 pruebas con el servidor local). El fallo nombra la fila ("Row 12: …") y la selecciona. Si la fila ya no existe o la ha cambiado otro, sale "changed by someone else" y no se guarda nada.
- [x] Edición completa por teclado: F2, Enter, Esc, Tab y Shift+Tab, insertar, borrar, Set to NULL, deshacer, rehacer, vista previa y Mod+S (`tst_data_tab`, 11 pruebas, que también cubren el walker, 200 %, alto contraste y español).
- Además:
  - **Indicadores** con icono y texto (Modified, New, Deleted, lápiz, "DEFAULT").
  - **Producción:** hay que escribir el nombre de la conexión; también lo comprueba el controlador.
  - **Claves sin PK:** `rowid`, `_rowid_` y `oid` en SQLite, `ctid` en PostgreSQL, con aviso. Las vistas y las tablas sin clave son de solo lectura.
  - **Guardas:** confirmación de cambios sin guardar al cerrar la pestaña, en F5, en Discard y al salir. Durante un guardado se bloquea todo.
  - **Pestañas de datos** no persistentes, y textos en español.
  - **Arranque sin coste medible:** A/B alternando binarios 9 veces, base 708 ms y H2 714 ms (mediana), con la máquina cargada y poca memoria.
  - **Suite completa en Windows Release** con PostgreSQL: 31/31.
- [ ] Pasada manual con lectores de pantalla de la pestaña de datos. **Pendiente**, junto con la de H1.5–H1.7.
- Limitaciones conocidas:
  - La PK se lee del detalle "PK" del árbol, porque `Session` no tiene `describe()` (ARCHITECTURE §6.4, `shortcut:`).
  - Las celdas BLOB son de solo lectura.
  - Set to NULL actúa solo sobre la celda actual.
  - Se borran como mucho 100 000 filas por paso.
  - Una fila insertada mientras la tabla carga queda tras las filas cargadas.
  - El historial de deshacer no tiene tope.
  - Cerrar la pestaña durante un guardado descarta la respuesta; la transacción es atómica igualmente.
  - Fetch all con edición solo se ha probado hasta 300 000 filas.

## H3 — Autocompletado
- Palabras clave del dialecto, esquemas, tablas, columnas (con alias), funciones; caché de metadatos por sesión, invalidable.
- Popup accesible (rol lista; se anuncia el número de sugerencias); Ctrl+Space.
- Decisiones (2026-11-15, ARCHITECTURE §6.5): metadatos bajo demanda con `Session::children()` y la sesión del árbol (columnas solo de las tablas que nombra la sentencia); solo funciones predefinidas (`ISqlDialect::builtinFunctions()`); el popup se abre solo tras `.` o tras 2 caracteres (150 ms, desactivable) y con Ctrl+Space; `@` en la paleta busca tablas y vistas en la misma caché.

Criterios:
- [x] Sugerencias en ≤ 50 ms con 10 000 tablas en caché. `bench_completion`, con 11 429 relaciones × 20 columnas, da p95 ≤ 0.58 ms y máximo 2.8 ms en todos los escenarios. En la app, `query()` sobre un archivo de 10 000 líneas da p95 7–8 ms.
- [x] Nunca bloquea la escritura. La consulta corre en el temporizador de 150 ms, fuera del camino de la tecla. Con 10 000 líneas y el popup abierto, la pulsación da p95 6–8 ms (5–7 ms sin popup), dentro del presupuesto de 16 ms.
- Además:
  - Contexto con alias, esquemas y comillas.
  - Funciones predefinidas por motor.
  - Popup accesible con anuncios, el ajuste "Suggest while typing" y el comando "Refresh metadata".
  - Invalidación tras DDL.
  - `@` en la paleta, que lleva al nodo del árbol.
  - La conformidad fija la ruta de la carpeta Columns.
  - Nunca pide la contraseña mientras se escribe.
  - Textos en español.
  - Arranque sin coste medible: A/B alternando binarios 9 veces, base 697 ms y H3 681 ms (mediana), con la máquina cargada.
  - Suite completa en Windows Release con PostgreSQL: 35/35, y `tst_controls` ×3 en verde.
- [ ] Pasada manual con lectores de pantalla (lista en UX.md, "Autocompletado"). **Pendiente**, junto con las anteriores.
- Limitaciones conocidas:
  - Sin ámbito por subconsulta.
  - Las funciones predefinidas son una lista parcial.
  - Una pestaña sin conexión no tiene dialecto, así que no hay palabras clave.
  - `@` busca solo por nombre, no por "esquema.nombre".
  - Al refrescar la caché, la lista vuelve a la primera fila.
  - Una carga de columnas que falla no se reintenta hasta invalidar la caché.

## H4 — Historial y consultas guardadas
- `query_history` con FTS5, búsqueda, filtro por conexión, re-ejecutar o copiar al editor. Consultas guardadas con carpetas.
- Opción de no guardar el historial por conexión (datos sensibles).
- Decisiones (2026-11-16):
  - UI: un panel lateral junto al árbol de esquema con dos pestañas, History y Saved; se abre con un comando y un atajo, y funciona solo con teclado.
  - Retención: se guardan éxitos y errores, con duración y filas. Tope de 10 000 entradas; al pasarlo se borran las más antiguas. Hay una acción "Clear history".
  - Privacidad: al activar "No guardar historial" en una conexión, deja de grabar y pregunta si borrar también lo ya guardado; sin respuesta afirmativa no se borra nada. Se añade `recordHistory` a `ConnectionProfile` (migración nueva sobre `connections`).
  - Consultas guardadas: globales, con nombre y SQL y una conexión asociada opcional. Carpetas de un solo nivel; se mueven con menú y teclado, sin arrastrar.
- Implementado (2026-11-16): contrato en ARCHITECTURE §11.3 y UX "Historial y consultas guardadas (H4)".
  - Core: migraciones 3 y 4, `QueryHistoryStore` (FTS5, tope de 10 000) y `SavedQueryStore`. sqlite3 de vcpkg ahora con la característica `fts5`.
  - App: `HistoryModel`, `SavedQueriesModel`, panel History/Saved, casilla de privacidad y diálogo de borrado, indicador "History off" en la barra de estado.
  - Tests: 38/38 C++ y smoke en Windows Debug; `tst_library.qml` 25/25 y `tst_completion.qml` 29/29 con `QT_QPA_PLATFORM=offscreen`.
  - CI en verde en los 3 sistemas (run 43980117614, `0ee1631`). Los fallos locales de `tst_data_tab.qml` y de `tst_controls` bajo ctest eran del entorno de esta máquina.
  - **Pendiente:** pasada manual con lectores de pantalla (History, Saved, diálogo de privacidad) y medir el arranque en una máquina en reposo (≤ 400 ms).

## H5 — Exportación
- CSV, TSV, JSON, JSON Lines, SQL `INSERT`, Markdown. Del resultado actual o en streaming directo desde la consulta (sin cargarla en el grid).

- Decisiones (2026-11-17):
  - Streaming: "Export…" vuelve a ejecutar la sentencia en una sesión propia, con fetch por lotes directo al archivo. No toca el grid ni la pestaña, y se puede cancelar. El alcance "resultado actual" exporta lo ya cargado.
  - Seguridad: el streaming solo admite sentencias que devuelven filas (`SELECT`, `WITH`, `TABLE`, `SHOW`, `EXPLAIN`) y pasa por `SqlSafety` como cualquier ejecución, para no repetir efectos de un `UPDATE` o `INSERT`.
  - Valores: CSV/TSV con NULL vacío, comillas RFC 4180, UTF-8 sin BOM (BOM opcional) y separador, cabecera y BOM configurables en CSV. JSON y JSON Lines con `null` real, números como números y binarios en base64. SQL `INSERT` con `NULL` literal y binarios en hex del dialecto. Markdown con NULL vacío y escape de `|`.
  - UI: diálogo "Export…" desde la pestaña Results, las pestañas de datos y la paleta, con formato, opciones, alcance y archivo; progreso con número de filas y cancelación, anunciados de forma accesible.
Criterios: exportar 10 M de filas en streaming con RAM constante (≤ +50 MB); se puede cancelar.

## H6 — MySQL / MariaDB
- MariaDB Connector/C: streaming con `mysql_use_result`, `KILL QUERY` para cancelar, `DELIMITER` en `splitStatements`. Conformidad en la CI con los servicios `mariadb:11` y `mysql:8.4`.

## H7 — SQL Server + ODBC genérico
- Provider ODBC (unixODBC / iODBC / Windows ODBC); SQL Server como perfil con su dialecto (T-SQL, `GO`); ODBC genérico con capacidades reducidas. Conformidad con `mcr.microsoft.com/mssql/server` en Linux.

## H8 — Túneles SSH
- libssh2: contraseña, clave y agente; `known_hosts` con confirmación de la huella. Credenciales en el llavero. Port forwarding local en un hilo propio.

## H9 — EXPLAIN visual
- PostgreSQL (`EXPLAIN (FORMAT JSON, ANALYZE opcional)`), MySQL (`EXPLAIN FORMAT=JSON`), SQLite (`EXPLAIN QUERY PLAN`). Árbol accesible con coste y filas; vista de texto alternativa.

## H10 — Distribución
- Windows: MSIX + instalador Inno Setup, firmado. macOS: `.app` universal (arm64 + x86_64 con `lipo`), `.dmg` firmado y **notarizado**. Linux: AppImage (construido en Ubuntu 24.04) + Flatpak.
- `windeployqt` / `macdeployqt` / `linuxdeploy` con la lista mínima de módulos; avisos de licencia LGPL de Qt y licencias de terceros en "About".
- Incluir `QtQuick.Dialogs` (Qt6QuickDialogs2 y su plugin QML) y, en Linux, resolver `libQt6DBus` para `libqt6keychain.so` (vcpkg le pone `RUNPATH=$ORIGIN`); hoy un `quarry` del árbol de build en Linux necesita `LD_LIBRARY_PATH`.

Criterios: tamaño de los paquetes dentro del presupuesto; instalación y arranque limpios en máquinas virtuales de las versiones mínimas (Win10 1809, macOS 14.4, Ubuntu 24.04).

## Sin fecha
- Oracle (ODPI-C; requiere Instant Client instalado por el usuario).
- Plugins de terceros con SDK estable.
- Windows arm64 y Linux arm64.
