# UX y accesibilidad de Quarry

Objetivo: **WCAG 2.2 AA**, uso completo con teclado y compatible con NVDA y Narrator (Windows), VoiceOver (macOS) y Orca (Linux). Cada componente nuevo cumple estas pautas desde el principio. Un componente que no las cumpla no se integra.

## 1. Principios

1. **Lo avanzado, solo cuando hace falta.** Los formularios muestran lo mínimo (host, puerto, usuario, base de datos) y el resto va en "Advanced options", plegado. Las acciones raras van en la paleta de comandos, no en la barra de herramientas.
2. **Los estados vacíos explican qué hacer.** Ninguna vista vacía se queda en blanco: dice qué es, por qué está vacía y qué hacer, con un botón para hacerlo. Ejemplo: "No results yet. Write a query and press Ctrl+Enter to run it."
3. **Errores en lenguaje claro.** Un título que dice qué pasó y qué hacer; el detalle técnico del driver va en una sección desplegable "Technical details" que se puede copiar (ARCHITECTURE §7).
4. **La información nunca se transmite solo con color.** El color va siempre acompañado de un icono, un texto o un patrón: producción, errores, NULL, celdas modificadas, estado de la conexión.
5. **Seguridad del usuario.** Producción en rojo con el texto "PRODUCTION" visible en la pestaña y en la barra de estado. Confirmación para `UPDATE`/`DELETE` sin `WHERE`. Vista previa del SQL antes de aplicar ediciones. Deshacer en el editor de datos. Las acciones destructivas nunca son el botón por defecto.
6. **Rápida y predecible.** Nada bloquea la interfaz. Las operaciones largas muestran su progreso y se pueden cancelar con la misma tecla en todas partes (Esc).
7. **La plataforma manda.** Se usan sus atajos (Cmd en macOS, Ctrl en Windows y Linux), sus tipografías, el escalado de fuente, el tema claro/oscuro, el alto contraste y la preferencia de reducir animaciones.

## 2. Requisitos de accesibilidad por componente

Cada control interactivo:
- Tiene `Accessible.role` correcto y `Accessible.name` traducible. Si el texto visible no basta (botones que solo tienen icono), añade `Accessible.description`.
- Es enfocable con teclado (`activeFocusOnTab`, o navegación interna con flechas en compuestos como el árbol, el grid o las pestañas).
- Muestra un **foco visible**: anillo de 2 px con contraste ≥ 3:1 frente al fondo, que no depende solo del color de fondo y no queda oculto por otros elementos (WCAG 2.4.7 y 2.4.11).
- Tiene un objetivo táctil o de clic ≥ 24×24 px lógicos (WCAG 2.5.8).
- Cumple el contraste de los tokens del tema: texto ≥ 4.5:1 (≥ 3:1 si es grande), bordes de controles y estados ≥ 3:1.
- Expone su estado: `checked`, `expanded`, `selected`, `disabled`, `busy`.
- Anuncia los cambios asíncronos relevantes con `Accessible.announce()` (Qt 6.8+), con prioridad *polite* salvo para los errores (*assertive*), y sin mover el foco.

Orden de tabulación: sigue el orden visual (de izquierda a derecha y de arriba abajo). Los diálogos atrapan el foco mientras están abiertos y lo devuelven al control que los abrió al cerrarse. **F6 / Shift+F6** recorre las regiones (árbol, editor, resultados, barra de estado).

## 3. Mapa de pantallas

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Barra de título/menú (macOS: menú global)                                │
├───────────────┬──────────────────────────────────────────────────────────┤
│ Sidebar       │ [● prod-db  PRODUCTION] [Query 2] [+]        ← pestañas  │
│ ─ Conexiones  ├──────────────────────────────────────────────────────────┤
│   ▸ local.db  │ Editor SQL                                               │
│   ▾ prod-db ● │                                                          │
│     ▾ public  │                                                          │
│       ▸ Tables├──────────────────────────────────────────────────────────┤
│               │ [Results] [Messages] [Plan]                              │
│ [+ New]       │ Grid de resultados virtualizado                          │
├───────────────┴──────────────────────────────────────────────────────────┤
│ Barra de estado: conexión · PRODUCTION · Running… 00:03 [Cancel] · 1 204 rows │
└──────────────────────────────────────────────────────────────────────────┘
```

| Pantalla | Contenido | Estado vacío |
|---|---|---|
| Bienvenida (sin conexiones) | Botones "New connection", "Open SQLite file" y atajos clave | Es en sí misma el estado vacío |
| Diálogo de conexión | Selector de motor → campos de `connectionFields()`, Advanced plegado, color, "Production", "Save password / Ask every time", "Test connection" | — |
| Sidebar / árbol | Un solo árbol: las conexiones guardadas son la raíz (con color y PRODUCTION) y su esquema se carga bajo demanda al desplegar | "No connections yet" + botón |
| Editor | Pestañas, números de línea, sentencia actual resaltada | Placeholder: "Write SQL here. Ctrl+Enter runs the current statement." |
| Resultados | Grid, contador, tiempo, "Fetch all" | "Run a query to see results here." |
| Mensajes | Errores, notices, filas afectadas | "No messages." |
| Editor de datos (H2) | Grid editable + barra de cambios pendientes (n cambios, Preview SQL, Apply, Discard) | — |
| Paleta de comandos | Campo de búsqueda + lista filtrada de comandos con su atajo | "No commands match '…'" |
| Ajustes | Tema (System/Light/Dark/High contrast), tamaño del texto, animaciones (System/On/Off), filas por página, idioma | — |
| Acerca de (H10) | Nombre, versión, descripción, copyright, licencia Apache-2.0, aviso de Qt (LGPLv3, enlace dinámico) y la vista "Third-party licenses" | — |
| Confirmaciones de seguridad | SQL afectado, conexión (con su marca de producción), botón destructivo explícito (no por defecto) | — |

### Árbol de esquemas (H1.4)

- Flechas ↑/↓ mueven; → despliega (o baja al primer hijo); ← pliega (o sube al padre); Home/End; Enter despliega o pliega (abrir una tabla llega con el grid en H1.7).
- Escribir letras salta al siguiente nodo visible cuyo nombre empieza por ese prefijo (se reinicia tras ~1 s sin teclear); no filtra.
- Mientras carga, el nodo muestra un hijo "Loading…" que se anuncia; si falla, un hijo de error con "Retry" y el detalle técnico.
- Desplegar una conexión conecta; plegarla no desconecta. "Disconnect" está en el menú contextual (Shift+F10 / tecla Menú) y en la paleta. Un nodo conectado lo indica con icono y texto, no solo con color.
- El lector de pantalla anuncia nombre, nivel, estado plegado/desplegado y posición ("Tables, expanded, 3 of 5"). Qt no tiene propiedades de nivel ni de posición en `Accessible`, así que van como texto en `Accessible.description` ("level 3, 1 of 2"); rol `TreeItem` y el estado expandido sí son nativos.
- Filas sintéticas: "Loading…" (anuncio educado); error (anuncio enérgico; Enter reintenta, Espacio despliega el detalle, Mod+C lo copia); "(empty)" cuando un nodo con hijos responde sin ninguno, para que nunca se anuncie "expanded" sobre nada.
- Los cambios al editar una conexión conectada se aplican al reconectar.

### Editor SQL (H1.5)

- Mod+T abre una pestaña asociada a la conexión del nodo actual del árbol (o a la de la pestaña activa). Sin ninguna, la pestaña queda sin conexión y muestra "Choose a connection"; la conexión se cambia desde la cabecera de la pestaña. La pestaña muestra el color y PRODUCTION de su conexión.
- Las pestañas y su texto se guardan solos y se restauran al abrir la app. Mod+W cierra la pestaña; si tiene texto, pide confirmación (cerrar borra su texto guardado).
- Mod+Enter / Mod+Shift+Enter: en H1.5 resaltan la sentencia actual / todas (con `splitStatements` del dialecto) y lo anuncian ("Statement 2 of 5, lines 4–7"); la ejecución llega en H1.6. La sentencia bajo el cursor siempre se ve resaltada.
- Números de línea y "Ln 12, Col 3" en la barra de estado. Buscar y reemplazar (Mod+F) llega en un hito posterior.
- Tab inserta un tabulador y Shift+Tab quita un nivel de sangría de la línea actual (un tabulador o hasta 4 espacios); aún no hay sangría de varias líneas. Para salir del editor sin ratón (WCAG 2.1.2): Ctrl+Tab / Ctrl+Shift+Tab (pestañas), F6 / Shift+F6, Mod+1/3 y Mod+K. El placeholder y la descripción accesible del editor lo indican.
- Cabecera de la pestaña: punto de color, título, PRODUCTION y botón de cerrar; debajo, la fila con el selector de conexión. Los títulos ("Query N", traducido) son únicos entre las pestañas abiertas.
- Diálogo de cierre: el foco empieza en Cancel; el botón destructivo es "Close and delete text".
- Anuncios: "Statement 2 of 3, lines 2–3", "3 statements", "Choose a connection first.", "Nothing to run.", "No query is open.". El resaltado de Mod+Enter (contorno de acento además del fondo) desaparece con la siguiente edición o movimiento del cursor. Un fallo al guardar muestra un aviso (`ErrorPanel`) y se anuncia de forma enérgica.
- Mod+Shift+Enter ejecuta todo; ejecutar solo la selección llega más adelante.

### Ejecución (H1.6)

- Mod+Enter ejecuta la sentencia actual y Mod+Shift+Enter todas, en orden, parando en el primer error. Antes de empezar, si hay sentencias peligrosas (`UPDATE`/`DELETE` sin `WHERE`; `DROP`/`TRUNCATE` en producción), un solo diálogo las lista con la conexión y PRODUCTION; el foco empieza en Cancel y el botón destructivo es explícito ("Run anyway").
- Barra de estado: "Running… 00:03" con el botón Cancel mientras se ejecuta, en una fila propia encima de "Connected / Selected / Ln" (en una sola fila no cabía al 200 %); al acabar, filas y tiempo ("120 rows · 15 ms" o "3 rows affected · 4 ms"; "500+ rows" si quedan filas por traer, con el tiempo total del plan). Cancelar: Esc en el editor o en los resultados, y Mod+. en cualquier sitio. Una cancelación muestra "Query cancelled", no un error.
- El inicio y el fin se anuncian de forma educada ("Running statement 2 of 5", "Done: 120 rows in 15 ms"); los errores, de forma enérgica. Nunca se mueve el foco. Si la pestaña no es la actual, el anuncio lleva su título delante ("Query 2: Query cancelled"). Rechazar la confirmación anuncia "Cancelled. Nothing was run."
- La región de resultados tiene las pestañas Results y Messages. Results muestra hasta H1.7 un resumen (filas, tiempo y nombres de columna). Messages lista el resultado de cada sentencia, las notices y los errores (con icono y texto del tipo, y "Technical details" desplegable y copiable). Con un error se pasa a Messages sin mover el foco.
- Si el error trae `position`, se subraya en el editor (línea continua de 2 px bajo el identificador o número que empieza en esa posición, mínimo un carácter, más un icono de alerta en el margen de números; no solo color). Solo se marca si el texto de la pestaña no ha cambiado desde que se ejecutó y se anuncia "Error at line 3, column 5".

### Grid de resultados (H1.7)

- Aparece con el primer resultado ("Run a query to see results here." antes). Cabecera fija; filas de altura fija; NULL se pinta como "NULL" en cursiva con `grid.null` (no solo color); los BLOB como `<binary 2.3 KB>`.
- Teclado: flechas, Home/End, Ctrl+Home/End y RePág/AvPág mueven; con Shift se extiende la selección (un rectángulo). Ctrl+A todo, Shift+Space la fila, Ctrl+Space la columna. Alt+Shift+←/→ cambia el ancho de la columna actual (y se anuncia "Column X, width N characters"); con el ratón, arrastrando el borde de la cabecera. Esc para Fetch all; si no hay, pasa a cancelar la consulta.
- Copiar: Mod+C (TSV sin cabecera) y Mod+Shift+C (con cabecera); se anuncia "Copied N cells". Más de 100 000 celdas: aviso y no se copia.
- Contador: "N rows", "N rows, more available", "N rows, loading all rows…", "N rows, not all loaded". El botón Fetch all solo se ve si quedan filas; mientras carga se llama "Stop fetching". Se anuncia "Loading all rows" y "All N rows loaded" (o "Stopped. N rows loaded").
- Lector de pantalla (plan B, ARCHITECTURE §13): el grid es una tabla con descripción de filas, columnas y teclas; cada movimiento anuncia "Row X of N, <columna>: <valor>".
- Pasada manual con NVDA/VoiceOver/Orca: Mod+3 → "Results grid" + descripción + primera celda; recorrer fila y columna (cabecera + valor, "NULL"); mantener ↓ (solo la última fila); Shift+flecha ("N cells selected") y Mod+C ("Copied N cells"); Tab a Fetch all, Espacio, "Loading all rows"/"All N rows loaded" sin perder el foco; Alt+Shift+←/→; sin lecturas duplicadas de celdas ni nodos "pane" sueltos con el cursor del lector; 200 % y alto contraste.

### Edición de datos (H2)

- **Abrir:** Enter o doble clic sobre una tabla, vista o vista materializada del árbol (o el comando "Open data") abre una pestaña de datos. Si la tabla ya está abierta, solo se le da el foco. Las pestañas de datos no se guardan en `quarry.db`. Su conexión no se puede cambiar.
- **Disposición:** la pestaña ocupa la región del editor, con la barra de herramientas, los avisos y el grid. La barra es un `Flow` que se reparte en varias líneas a 200 %. Tiene Insert row, Delete rows, Set to NULL, Undo, Redo, Preview SQL, Save changes, Discard changes, Refresh y el texto "N pending changes". La región de resultados solo muestra Messages.
- **Teclado en el grid:** F2 o Enter editan la celda actual. Una tecla imprimible empieza a editar y reemplaza el contenido. En el editor de celda:
  - Enter confirma y baja.
  - Tab / Shift+Tab confirman y van a la derecha / izquierda.
  - Esc cancela.
  - Perder el foco confirma.
  - Doble clic también edita.
- **Celdas bloqueadas:** una celda BLOB, de fila borrada o de vista anuncia el motivo.
- **Indicadores** (nunca solo color):
  - La columna 0 tiene un chip con icono y texto: Modified, New o Deleted. La fila borrada además se tacha.
  - Una celda cambiada lleva un icono de lápiz y un tinte.
  - Una celda sin valor de una fila nueva muestra "DEFAULT" en cursiva.
  - El anuncio incluye el estado: "Row 3 of 50, name: Alice, modified".
- **Solo lectura:** el motivo aparece en un aviso con candado. Una tabla sin PK que se edita por `rowid` o `ctid` muestra un aviso con triángulo, que también se anuncia. Si una columna llamada `rowid` tapa el alias en SQLite, se usa `_rowid_` y después `oid`; si las tres existen, la tabla es de solo lectura.
- **Vista previa y guardado:** Preview SQL muestra cada sentencia con sus parámetros, exactamente lo que se ejecutará. Save (Mod+S) aplica todo en una transacción.
  - En una conexión de producción hay que escribir el nombre de la conexión. El botón peligroso está desactivado hasta que coincide, y el controlador también lo comprueba.
  - Mientras se guarda, todo queda bloqueado: edición, Refresh, Discard y un segundo Save. Se muestra "Saving…", que también se anuncia.
  - Si falla, nada se guarda y los cambios siguen pendientes. El mensaje dice qué fila falló ("Row 12: …" o "New row 2: …"), y esa fila se selecciona.
- **Cambios sin guardar:** cerrar la pestaña, Refresh (F5), Discard y salir de la app piden confirmación con Discard / Cancel; el foco empieza en Cancel. Cerrar durante un guardado está permitido: la transacción termina o se revierte en el hilo de la sesión, y su respuesta se descarta.
- **Límites:** no se borran más de 100 000 filas de una vez (se anuncia por qué). Set to NULL actúa solo sobre la celda actual. Una fila insertada mientras la tabla aún carga queda tras las filas cargadas. El historial de deshacer no tiene tope.

### Vista de datos de tabla (H11, fase 1)
- **Barra "Sort and filter"** (rol ToolBar) bajo la de edición, en orden de tabulación: "Sort by" (No order + columnas), "Sort direction" (Ascending / Descending, activo solo con orden), "Filter column" (No filter + columnas), "Filter condition" (equals, does not equal, contains, starts with, is less than, is at most, is greater than, is at least, is NULL, is not NULL), "Filter value" (activo solo si la condición lo necesita; Enter aplica), Apply y Clear. Todos con nombre accesible propio.
- La vista activa se resume en texto junto a la barra ("Sorted by name, descending. Filter: note contains "x""), no solo con el estado de los combos.
- Aplicar relanza la lectura en el servidor con el anuncio de siempre ("Running statement…") y se cancela con Esc / Mod+.. Con cambios pendientes no se aplica y se anuncia de forma enérgica "Save or discard the pending changes before sorting or filtering.".
- Las pestañas de datos piden 200 filas por página; el resto llega al desplazarse (como en H1.7) o con "Fetch all".

### Autocompletado (H3)

- **Popup:**
  - Lista de sugerencias junto al cursor: debajo del caret, o encima si abajo no cabe y arriba hay más sitio. Funciona a 200 % y en alto contraste.
  - Cada fila muestra el icono, el nombre, el tipo en palabra (keyword, function, schema, table, view, column) y un detalle: el tipo de la columna, "tabla: tipo" o el esquema de la tabla.
  - Es una lista accesible (rol List, filas ListItem, nombre "Suggestions"). Nunca toma el foco del teclado: lo conserva el editor.
- **Apertura:** se abre sola 150 ms después de `.` o de 2 o más caracteres de identificador; nunca dentro de strings, comentarios o números. Ctrl+Space siempre la abre: es el comando "Show suggestions" (categoría Editor), y en macOS es Control+Space porque Cmd+Space es Spotlight. El ajuste "Suggest while typing" (activado por defecto) desactiva la apertura automática, y Ctrl+Space sigue funcionando.
- **Teclas:**
  - Arriba, Abajo, RePág, AvPág, Inicio y Fin mueven por la lista.
  - Enter y Tab aceptan. Tab solo acepta con el popup abierto; si no, sangra.
  - Esc cierra solo el popup, y con él abierto no cancela la consulta en curso.
  - Seguir escribiendo filtra. Mover el cursor fuera de la palabra lo cierra.
- **Aceptar** reemplaza la palabra en un solo paso de deshacer, incluida una comilla de cierre que ya estuviera tras el cursor.
- **Anuncios** (con 250 ms de espera): "N suggestions" al abrir o cuando cambia el número, más la fila actual ("users, table"). Si Ctrl+Space no encuentra nada, se dice "No suggestions".
- **Sin sesión utilizable**, solo hay palabras clave y funciones, y el popup muestra "Connect to suggest tables". Una conexión que pediría la contraseña nunca se abre mientras se escribe.
- **Metadatos:** se leen la primera vez que se usa el autocompletado en una conexión: primero las tablas, y las columnas de una tabla solo cuando la sentencia la nombra.
  - El comando "Refresh metadata" (`connection.refreshMetadata`) vacía la caché de la conexión de la pestaña.
  - Un CREATE, ALTER, DROP o RENAME que termina bien también la vacía, igual que reintentar en el árbol.
- **Paleta con `@`:** lista las tablas y vistas de la conexión de la pestaña, con el tipo en palabra y el esquema. Enter selecciona el nodo en el árbol, desplegando las ramas, y lleva allí el foco. Los estados vacíos se anuncian como texto.
- **Pasada manual con lector de pantalla:** abrir con `.` y con Ctrl+Space, recorrer la lista ("N suggestions" y la fila actual, sin perder la lectura del editor), aceptar con Enter y con Tab, y Esc. Repetirlo a 200 % y en alto contraste.

### Historial y consultas guardadas (H4)
- **Ubicación:** el sidebar queda con el árbol arriba y un panel con las pestañas History y Saved abajo, separado por un `QSplitView` que se redimensiona con teclado. Empieza cerrado y no se persiste. Sigue habiendo 4 regiones de F6; Tab o la tecla Menú llegan al panel. Esc lo cierra y devuelve el foco.
- **Atajos:** Mod+Shift+H (History) y Mod+Shift+L (Saved) muestran el panel, lo enfocan y, si ya estás en esa pestaña, lo cierran. Mod+Shift+S guarda la consulta actual. "Clear history" no tiene atajo. Los cuatro comandos están en la paleta (categoría "History") y en el menú.
- **Historial:** rol List. Cada entrada muestra el SQL, la hora, la duración y las filas; las fallidas llevan icono, la palabra "Failed" y el mensaje. Enter (o doble clic) copia la sentencia al final del editor sin ejecutar; Run la ejecuta (en una pestaña nueva si la actual es de otra conexión); Mod+C copia el SQL; Supr borra la entrada. Búsqueda con retardo de 200 ms, filtro "Only this connection", "Only errors" y "Load more". No se guardan las canceladas ni los SELECT de las pestañas de datos.
- **Guardadas:** rol Tree/TreeItem, dos niveles, con "nivel, N de M" en la descripción. Flechas expanden y contraen, Enter abre la consulta o alterna la carpeta, F2 renombra, Supr borra, Shift+F10 o Menú abren el menú. Mover entre carpetas con el diálogo "Move to…", sin arrastrar. Borrar una carpeta mueve sus consultas al nivel superior y el diálogo lo dice.
- **Privacidad:** la casilla "Don't save query history for this connection" está en el formulario de conexión. Al guardar una conexión que grababa y ya no, un diálogo pregunta si borrar también el historial existente: "Keep history" (por defecto) o "Delete history". La barra de estado muestra el icono y el texto "History off" junto a la conexión de la pestaña.
- **Anuncios:** "N history entries", "No history entries", "History cleared", "History entry deleted", "SQL copied", "Copied to Query 1", "Query “X” saved", "Folder deleted. N queries moved to the top level". Los diálogos destructivos enfocan Cancel.
- **Límites:** el texto copiado al editor recibe un `;` final si falta (salvo que la última línea acabe en un comentario `--`). Una entrada nueva no se filtra por el texto de búsqueda hasta la siguiente búsqueda. El indicador "History off" no aparece en las filas del árbol de conexiones.

### Exportación (H5)
- **Dónde:** Mod+Shift+E, el comando "Export…" de la paleta (`export.open`, categoría "Export"), un botón en la barra del panel de resultados y un botón "Export…" en la barra de las pestañas de datos. Si no se puede exportar, el botón queda deshabilitado y el motivo está en el tooltip y en la descripción accesible.
- **Diálogo:** una vista de formulario y una de progreso en la misma ventana. Formato: lista con una sola selección (Arriba/Abajo/Inicio/Fin). Las opciones cambian con el formato (CSV: separador, cabecera y BOM; TSV y Markdown: cabecera; SQL INSERT: tabla y filas por INSERT). Alcance: "Current result" (lo cargado en el grid) o "Run the query again" (streaming completo con una sesión propia). Archivo: campo de ruta y "Browse…", que abre el selector nativo. Si el archivo existe, pide confirmar el reemplazo con el foco en Cancel.
- **Progreso:** el contador de filas se actualiza en pantalla, pero solo se anuncian el inicio, el final, la cancelación y el error, para no saturar al lector de pantalla. Esc cancela una exportación en curso y cierra el diálogo cuando ya terminó. El foco pasa a Close al terminar y a Back tras un error o una cancelación. Al acabar: "Exported N rows (X MB)" con "Show in folder".
- **Límites:** "Run the query again" solo admite sentencias que devuelven filas (`SELECT`, `WITH … SELECT`, `TABLE`, `SHOW`, `EXPLAIN` sin efectos); `SELECT … INTO`, CTE que modifican datos, `PRAGMA` y `VALUES` quedan fuera. Una función con efectos dentro de un `SELECT` no se detecta. En una pestaña de datos se exporta `SELECT *` de la tabla, sin orden ni límite de página, y las ediciones pendientes no se incluyen; el diálogo lo dice. Con una conexión "Ask every time" sin contraseña en memoria hay que ejecutar antes una consulta en la pestaña. El progreso muestra filas; los bytes solo se conocen al final.
### Túnel SSH (H8)
- **Formulario:** la sección "SSH tunnel" del diálogo de conexión solo existe si el motor tiene `Capability::Ssh` (PostgreSQL, MySQL y SQL Server; no SQLite ni ODBC). Va debajo de la contraseña de la base de datos. Empieza con la casilla "Connect through an SSH tunnel"; con ella marcada aparecen "SSH host", "SSH port" (22), "SSH user" y "SSH authentication" (Password, Private key, SSH agent). Con Private key se añade "Private key file" con "Browse…" (el mismo selector de archivos que SQLite, con su propio título); Quarry solo guarda la ruta y nunca lee ni copia la clave. Con Password o Private key hay además un selector "SSH secret storage" ("Save in the system keychain" o "Ask every time") y el campo de la contraseña SSH o de la frase de paso (opcional), con el mismo modelo que la contraseña de la base de datos: campo vacío conserva el secreto guardado, "Ask every time" no guarda nada y solo sirve para probar. Con SSH agent no hay secreto (Pageant y agente de OpenSSH en Windows; `SSH_AUTH_SOCK` en Linux y macOS).
- **Validación:** los datos no secretos van a `profile.values` con las claves `ssh.*` de `SshTunnelConfig`, más `ssh.secretMode` (`keychain` o `ask`). Los campos obligatorios (host, usuario y, con clave, el archivo) y el puerto (1 a 65535) se validan en vivo con las mismas reglas que `SshTunnelConfig::validationProblem()`. El mensaje sale junto al campo con icono y texto (nunca solo color), se anuncia con `Accessible.announce` y el motivo de Guardar pasa a "Fill in the SSH tunnel settings to save.". Nada se muestra antes de que el usuario interactúe, salvo al guardar o probar.
- **Orden de Tab:** casilla, host, puerto, usuario, método, archivo de clave (y Browse…), almacenamiento del secreto, secreto. Todo funciona solo con teclado; la casilla se alterna con Espacio.
- **Secretos:** SSH password y Key passphrase viven en el llavero (`connection/<uuid>/ssh-password` y `…/ssh-passphrase`) o solo en memoria ("Ask every time"). Al cambiar de método, de modo o de conexión a una sin túnel se borra la entrada que ya no corresponde; borrar la conexión borra las tres. Con "Ask every time" aparece el diálogo de contraseña con otros textos: "SSH password required" o "Key passphrase required" ("Leave it empty if the key has none"). Si el servidor rechaza el inicio de sesión SSH, el secreto recordado se olvida y el siguiente aviso dice "The SSH password was not accepted. Try again.".
- **Diálogo de huella (`HostKeyDialog`):** antes de crear la conexión (árbol, pestañas de consulta, exportación y "Test connection") se comprueba la huella del servidor SSH.
  - *Desconocido:* "The authenticity of the server <host> can't be verified." con el tipo de clave y la huella SHA256 en la fuente monoespaciada del tema, seleccionable y copiable (Mod+A, Mod+C) y con su texto completo en `Accessible.description`. Acciones: "Cancel" (el foco empieza aquí), "Trust once" (vale hasta cerrar Quarry) y "Trust and remember" (se escribe en el `known_hosts` propio de Quarry, nunca en `~/.ssh`). Tab recorre Cancel, Trust once, Trust and remember y la huella. Esc o Cancel cancelan la conexión sin mensaje de error.
  - *Cambiada:* nunca se conecta ni se puede aceptar desde aquí. Se muestra un diálogo de aviso, con icono y texto, con las huellas conocida y recibida y un único botón "Close" (con el foco), y además el error normal de la conexión con la explicación del riesgo y las dos huellas. Lo mismo ocurre si el túnel detecta el cambio al abrir (carrera): sin reintento automático.
  - La exportación no tiene diálogo, así que no pregunta: con una huella desconocida o un secreto "Ask every time" sin memorizar termina con un error que dice dónde confirmarlo (conectar antes desde la lista).
- **Errores:** los fallos del túnel (autenticación, clave ilegible, servidor inalcanzable, huella) llegan como `Error` en lenguaje claro y salen donde ya salen los de conexión: panel de error con "Technical details", fila de error del árbol, mensaje de la consulta o de la exportación, y el resultado del botón "Test connection" con una pista ("Check the SSH user name and the password or the key."). Un fallo de huella no lleva pista.
- **Lista de conexiones:** la conexión con túnel lo dice con texto, no con color: el resumen termina en "(via SSH <host>)" y eso entra en la descripción accesible del nodo.
- **Límites:** el túnel no marca sus errores de autenticación, así que se distinguen de los de la base de datos por no tener SQLSTATE (ver `ConnectionPreflight::isSshAuthError`). Con dos conexiones que preguntan a la vez, los avisos se encolan: uno cada vez. El autocompletado nunca abre avisos: si la huella no está confirmada, la conexión del árbol falla con el mensaje que explica cómo hacerlo. Pendiente de probar con lectores de pantalla (NVDA, Narrator, VoiceOver, Orca) y con un servidor SSH real.

### Plan de consulta (H9)

- **Acciones:** "Explain current statement" (`editor.explain`, **Mod+E**) y "Explain analyze current statement" (`editor.explainAnalyze`, **Mod+Shift+A**), también en la paleta, en el menú y como dos `IconButton` en la fila de la conexión del editor. Mod+Shift+E ya es Exportar y Ctrl+Alt+E es AltGr+E (el euro) en muchos teclados, así que "Analyze" lleva la A. Actúan sobre la misma sentencia que Mod+Enter (la que está bajo el cursor; ejecutar solo la selección sigue pendiente) y la marcan igual. Solo se activan en una pestaña de consulta con conexión cuyo motor tenga `Capability::Explain`. Si el motor no tiene EXPLAIN para esa sentencia o modo (`ISqlDialect::explainStatement` devuelve vacío; SQLite no tiene analyze), no se envía nada: el motivo ("SQLite can't run Explain analyze. Use Explain to see the estimated plan.") se anuncia y se escribe en la pestaña Plan.
- **Explain (plan estimado):** no ejecuta la sentencia, ni siquiera un `INSERT`, `UPDATE`, `DELETE` o DDL: solo pide el plan. Va por la misma sesión que Run (misma contraseña, huella SSH y cancelación), se anuncia "Getting the query plan" y "Query plan ready, N steps", y se guarda en el historial como lo que de verdad se ejecutó (el `EXPLAIN …`), igual que cualquier sentencia.
- **Explain analyze:** el motor ejecuta la consulta entera para medirla. Solo se ofrece para lecturas sin efectos (`SELECT`, `WITH … SELECT`; sin `INSERT`/`UPDATE`/`DELETE`/`MERGE` en una CTE ni `SELECT … INTO`: `SqlSafety` no tiene esa comprobación y la hace `QueryController`). Para escrituras y DDL la acción dice por qué no ("Explain analyze runs the statement, so it only takes SELECT queries. Use Explain to see the plan without running it.") y no ejecuta nada. **Siempre** pide confirmación (`ConfirmAnalyzeDialog`): explica que la consulta se ejecutará hasta el final y puede tardar lo que ella, muestra la conexión (con PRODUCTION si lo es) y la sentencia, el foco empieza en Cancel y el botón es "Run and analyze". Esc o Cancel no ejecutan nada ("Cancelled. Nothing was run.").
- **Pestaña Plan:** aparece junto a Results y Messages cuando se pide un plan en esa pestaña del editor, y se va con ella (cada pestaña del editor tiene su plan: al cambiar de pestaña solo se ve el de la actual; cambiar la conexión de la pestaña o cerrarla borra el suyo). Pedir un plan activa la pestaña y lleva el foco a ella: al árbol en cuanto el plan llega (mientras tanto, a un estado "Getting the query plan…" que Esc cancela, como cualquier consulta; la barra de estado y Mod+. también). Ejecutar una consulta normal trae Results al frente pero **no** borra el plan, que se queda hasta el siguiente Explain; Explain tampoco borra la rejilla de Results. Un error del propio EXPLAIN sale en la pestaña Plan (y en Messages), sin saltar a Messages ni subrayar el editor.
- **Cabecera:** motor, "estimated: the query was not run" o "analyzed: the query was run", número de pasos y, si el motor los da, tiempos de planificación y ejecución; debajo, qué miden las barras ("share of the total execution time" / "estimated cost", o "<motor> gives no cost or time, so there are no bars."). Si el plan se cortó en 5000 pasos, un aviso con icono y texto ("Showing the first 5,000 steps").
- **Árbol:** un `TreeView` (como el de esquemas), todo desplegado, con la raíz sin etiqueta de SQLite oculta. Cada fila: etiqueta del paso en negrita y debajo, en una línea que se parte en pantallas estrechas, la barra proporcional con su porcentaje **en texto** ("34%", "<1%"), las filas ("1,234 rows", "est. 1,234 rows", "× 3 loops") y el tiempo real ("0.42 ms") o, sin analyze, el coste ("cost 12.5"); si hay avisos, un icono y "N warnings". Sin diagrama de cajas y flechas.
- **Barras y porcentajes:** el porcentaje de un paso es su parte propia (el total del paso menos la suma de los totales de sus hijos, nunca menos de 0) sobre el total del plan (el de la raíz, o la suma del primer nivel si la raíz no se ve). Con analyze y tiempos reales se usa el tiempo, si no el coste estimado; si el motor no da ni uno ni otro (SQLite) no hay barra ni porcentaje: no se inventa. Los costes son del motor y no se comparan entre motores. La barra es decorativa: el porcentaje y los números están siempre en texto, y la fila seleccionada pinta la barra con los colores de la selección. Números con la configuración regional del sistema.
- **Teclado (árbol):** ↑/↓ mueven, RePág/AvPág saltan, Home/End primer y último paso, → despliega y, si ya está desplegado, entra en el primer hijo, ← pliega y, en una hoja o en un paso plegado, sube al padre, Enter y Espacio pliegan/despliegan. Sin escritura predictiva. Tab sigue: cabecera/pestañas Tree y Text, árbol, lista de detalles; Esc cancela mientras se hace el plan.
- **Detalle del paso:** junto al árbol, una lista con el paso actual: parte, coste, filas, tiempo, sus avisos (icono y "Warning: …", no solo color) y los detalles clave-valor del motor ("Filter", "Index Cond"…) en la fuente monoespaciada. Es una lista enfocable (Tab) con nombre "Details of <paso>".
- **Vista Text:** la pestaña "Text" muestra la salida original del motor, seleccionable y copiable (Mod+A, Mod+C y el botón "Copy text"; las flechas, RePág/AvPág y Home/End la desplazan). Se crea al mostrarla, no antes. Si el plan no se pudo leer (`parseExplain` falla), la pestaña muestra el error en lenguaje claro con "Technical details" y la salida del motor en la vista Text: nunca queda vacía.
- **Accesibilidad:** el árbol tiene rol `Tree` y nombre "Query plan"; cada fila es un `TreeItem` con nombre = etiqueta y descripción con todo ("20% of the total execution time, cost 100, 1,200 rows, 30 ms, warning: Rows estimate off by 10x, level 2, 1 of 2"), expandible/expandido/seleccionado, foco visible. Llegada y fallo se anuncian ("Query plan ready, N steps", error de forma enérgica); la cancelación y el rechazo, con "Query cancelled" y el motivo. Todo con tokens de `Theme` (la barra es `accent` sobre `bg.hover`; los avisos, `warning`; no se añadieron colores), legible en claro, oscuro y alto contraste.
- **Límites:** 5000 pasos como máximo (los parsers cortan por anchura y marcan `truncated`) y 200 niveles. El texto del plan se lee en el hilo de la UI hasta 256 KiB (unos milisegundos) y en un hilo propio por encima, sin bloquear la interfaz; por encima de 32 MiB no se guarda y se avisa. Un EXPLAIN se pide con hasta 100 000 filas (los motores devuelven unas pocas; los JSON, una). Con un motor sin `Capability::Explain` (SQL Server, ODBC) las acciones están desactivadas. No verificado con lectores de pantalla ni con planes reales de PostgreSQL o MySQL.

### Acerca de (H10)

- **Dónde:** el comando "About Quarry" (`about.open`, categoría "General"), en la paleta y en el menú de la app. No tiene atajo: se usa poco y F1 se reserva para una ayuda futura. La versión se muestra solo aquí (no en la barra de título) y sale de `Qt.application.version`, que se fija con `project(VERSION)`.
- **Diálogo (`AboutDialog`):** icono, "Quarry", "Version X.Y.Z", una línea de descripción, "Copyright 2026 The Quarry authors", la licencia Apache 2.0 y el aviso de Qt (LGPLv3, enlazado dinámico). El foco empieza en Close; Esc cierra y el foco vuelve a donde estaba. El diálogo se crea al abrirlo por primera vez.
- **Licencias de terceros:** el botón "Third-party licenses" cambia a una vista de texto de solo lectura con scroll, seleccionable y copiable, con las mismas teclas que la vista Text del plan (Mod+A, Mod+C, flechas, RePág/AvPág, Inicio/Fin) y el botón "Copy text". "Back" vuelve a la vista principal (el foco pasa al botón de licencias) y Esc cierra el diálogo. Tab recorre el texto, Back, Copy text y Close. El texto sale de `AboutInfo.licenseText()`, que lee el recurso `:/licenses/THIRD_PARTY_LICENSES.txt` solo cuando se abre esa vista; si el recurso no existe muestra un aviso de "not available" en lugar de fallar.
- **Accesibilidad:** el texto de licencias es un `EditableText` de solo lectura y multilínea con nombre "Third-party licenses"; el icono es decorativo (`Accessible.ignored`); copiar se anuncia ("Licenses copied"). Solo tokens de `Theme`. Cabe a 200 % de zoom (el diálogo no pasa del tamaño de la ventana y la vista de licencias se encoge y hace scroll). Pendiente de probar con lectores de pantalla.

## 4. Atajos de teclado

`Mod` = Ctrl en Windows/Linux y Cmd en macOS. Se usa `QKeySequence::StandardKey` cuando existe uno equivalente.

| Acción | Atajo |
|---|---|
| Paleta de comandos | Mod+K (alternativo: Mod+Shift+P) |
| Ejecutar sentencia actual | Mod+Enter (también con el Enter del teclado numérico) |
| Ejecutar todo / selección | Mod+Shift+Enter (ídem) |
| Plan de consulta (H9) / plan con ANALYZE | Mod+E / Mod+Shift+A |
| Cancelar ejecución | Esc (en el editor/resultados) · Mod+. |
| Nueva pestaña de consulta | Mod+T |
| Cerrar pestaña | Mod+W |
| Pestaña siguiente / anterior | Ctrl+Tab / Ctrl+Shift+Tab |
| Nueva conexión | Mod+Shift+N |
| Abrir archivo SQLite | Mod+O |
| Recorrer regiones | F6 / Shift+F6 |
| Ir a árbol / editor / resultados | Mod+1 / Mod+2 / Mod+3 |
| Buscar en el editor / en el grid | Mod+F |
| Formatear SQL (hito posterior) | Mod+Shift+F |
| Autocompletar (H3) | Ctrl+Space (en macOS, Control+Space); solo en el editor: en el grid, Ctrl+Space selecciona la columna |
| Editar celda (H2) | F2 o Enter · Esc cancela |
| Deshacer / rehacer | Mod+Z / Mod+Shift+Z (en Windows también Ctrl+Y) |
| Insertar fila / borrar filas (H2) | Mod+I o Alt+Ins / Mod+Supr |
| Poner a NULL (H2) | Mod+Shift+Retroceso |
| Vista previa del SQL / guardar cambios (H2) | Mod+Shift+D / Mod+S |
| Recargar la pestaña de datos (H2) | F5 |
| Menú contextual | Shift+F10 / tecla Menú |
| Ajustes | Mod+, |
| Aumentar / reducir / restablecer zoom | Mod+= / Mod+- / Mod+0 |

Editar, duplicar, desconectar y borrar conexión (`connection.edit`, `connection.duplicate`, `connection.disconnect`, `connection.delete`) no tienen atajo global: se usan desde la paleta o el menú contextual del árbol, y actúan sobre la conexión a la que pertenece el nodo actual. La tecla Supr borra solo sobre una fila de conexión. `connection.disconnect` solo se activa si esa conexión está conectada. Los comandos `data.*` (categoría "Data") solo se activan en una pestaña de datos. `data.open` y `data.discard` no tienen atajo. Mientras una celda se edita, deshacer, rehacer, insertar, borrar y poner a NULL se desactivan, para que esas teclas lleguen al campo de texto. `editor.runStatement` y `editor.runAll` se desactivan en las pestañas de datos. `editor.explain` y `editor.explainAnalyze` solo se activan en una pestaña de consulta con una conexión cuyo motor sabe mostrar un plan.

Todos los atajos aparecen en la paleta de comandos junto a su comando; así se pueden descubrir sin documentación. Un atajo no puede ser solo una letra sin modificador fuera de los campos de texto (WCAG 2.1.4).

## 5. Paleta de comandos

- `CommandRegistry` (C++) guarda los comandos: `commandId` (en QML `id` está reservado), `title` (traducible), `category`, `shortcut` y un atajo alternativo opcional, `enabled`, `trackRecent` y `trigger()`. Los menús, los atajos y la paleta usan esa misma fuente. El texto de los atajos sale de `CommandRegistry.displayShortcut()`, que usa el modificador nativo de cada plataforma (⌘ en macOS); nunca se escribe "Ctrl+" a mano.
- Menú de la app: un botón de menú (IconButton "Menu") abre un `QMenu` con los mismos comandos, en todas las plataformas, porque la barra de menús nativa de macOS exigiría Qt.labs.platform. La paleta y los Ajustes se crean al usarse por primera vez o tras 1,5 s de inactividad, para no penalizar el arranque.
- Búsqueda difusa por título y categoría; los comandos usados recientemente salen primero.
- También sirve para saltar a objetos: con el prefijo `@` busca tablas y vistas en la caché de metadatos de la conexión activa (H3).
- Accesibilidad: un campo con rol `EditableText`, una lista con rol `List` y el elemento activo con `ListItem`. Al filtrar se anuncia "N commands". Las flechas mueven el elemento activo sin quitar el foco del campo. Enter ejecuta y Esc cierra y devuelve el foco.

## 6. Sistema de temas

- Tokens semánticos en un singleton QML `Theme` respaldado por `ThemeManager` (C++): `bg.canvas`, `bg.surface`, `bg.raised`, `fg.default`, `fg.muted`, `fg.onAccent`, `border.default`, `border.strong`, `focus.ring`, `accent`, `danger`, `warning`, `success`, `selection.bg`, `selection.fg`, `grid.null`, `conn.<color>`, `conn.production` y los del resaltado SQL `syn.keyword`, `syn.string`, `syn.number`, `syn.comment`, `syn.function`, `syn.type` y `syn.operator` (≥ 4.5:1 sobre `bg.canvas` y sobre `bg.hover`, el fondo de la sentencia actual; los errores usan `danger`; en el alto contraste del sistema todos valen el color del texto y los comentarios van en cursiva). Los componentes nunca usan colores literales.
- Temas: **Light**, **Dark** y **High contrast** (claro y oscuro). En el modo "System" se siguen:
  - `QStyleHints::colorScheme` (claro/oscuro) con `colorSchemeChanged`.
  - `QStyleHints::accessibility()->contrastPreference()` (Qt 6.10+) para el alto contraste. En Windows, el modo de alto contraste usa los colores del sistema (`QPalette`).
  - `QStyleHints::accessibility()->motionPreference()` (Qt 6.12) para reducir animaciones: `Theme.animationDuration` pasa a 0 y se quitan las transiciones que no son esenciales.
- **Acento** (decisión del 2026-11-13): azul índigo. Lo usan la acción principal, la selección y el foco. Cada tema define su tono para cumplir ≥ 4.5:1 como texto y ≥ 3:1 como indicador. El rojo queda reservado para producción y errores, y el verde para éxito.
- **Densidad:** compacta por defecto (filas y controles de ~24 px). En Ajustes hay una opción "Comfortable" (~32 px). Los objetivos de clic nunca bajan de 24×24 px (WCAG 2.5.8). Se expone como `Theme.density` y las medidas se derivan de ella.
- **Iconos:** [Lucide](https://lucide.dev) (ISC) como fuente, recortada a los iconos usados (`scripts/icons.txt` → `scripts/subset_icons.py` → `src/app/assets/icons/lucide-subset.ttf` + `Icons.qml`). Se pintan con `Text` y toman el color del token del tema, así que funcionan en alto contraste, escalan con el zoom y no necesitan Qt Svg. Un icono nunca va solo: los botones que solo tienen icono llevan `Accessible.name` y un tooltip.
- Tipografía: base = fuente del sistema (`QGuiApplication::font()`). Todas las medidas se derivan de `Theme.fontSize` y del zoom de la app (Mod+=/-), y el layout aguanta hasta un 200 % sin cortar texto (WCAG 1.4.4). Para el código, una fuente monoespaciada del sistema configurable.
- Colores de conexión: una paleta de 8 colores distinguibles también con daltonismo: Blue, Orange, Teal, Purple, Pink, Gold, Green y Slate. Cada color va acompañado de su nombre y de un icono. **No hay rojo** (decisión del 2026-11-13): el rojo solo significa peligro o producción, y "Production" siempre añade además el texto y el icono de aviso.
- Separadores de paneles (decisión del 2026-11-13): el tirador de `QSplitView` es fino (8 px) para aprovechar el espacio. Para cumplir WCAG 2.5.8 por la excepción de *control equivalente*, cada panel se puede mostrar u ocultar y el diseño se puede restablecer con comandos de la paleta y del menú Ver, y con botones de 24 px en la barra. El tirador también se redimensiona con el teclado (flechas).
- Un test unitario recorre todos los pares token-fondo de cada tema y comprueba los ratios WCAG (ROADMAP H1.2).

## 7. Cómo se prueba la accesibilidad

**Automático (CI, cada PR):**
- Test QML que recorre el árbol de items de cada pantalla y falla si un item enfocable no tiene `Accessible.name` o tiene rol `NoRole`.
- Test de orden de foco: simula Tab y Shift+Tab y compara la secuencia con la esperada en cada pantalla.
- Test de contraste de los tokens (§6).
- Test de teclado: cada comando del `CommandRegistry` se puede ejecutar sin ratón (atajo o paleta).
- Linux: un test con `QT_ACCESSIBILITY=1` y AT-SPI (`dbus-run-session` + `at-spi2-registryd`) comprueba que el árbol accesible expone los nombres esperados en las pantallas principales.

**Manual (al cerrar cada hito que toque UI), lista de comprobación en `docs/a11y-checklist.md`:**

| Lector | Plataforma | Comprobaciones |
|---|---|---|
| NVDA (última estable) | Windows 11 | Recorrer la app con Tab y F6; árbol (nivel, plegado/desplegado, posición); grid (cabecera + valor por celda); anuncios de ejecución y errores; diálogos |
| Narrator | Windows 11 | Lo mismo, por cubrir UIA sin NVDA |
| VoiceOver | macOS 14+ | Rotor (encabezados, controles), VO+flechas en el grid, menús globales |
| Orca | Ubuntu 24.04 (GNOME) | Mismas rutas que NVDA |

También en cada hito de UI: modo de alto contraste de Windows, zoom de texto del 200 %, "reducir movimiento" activado y navegación solo con teclado de punta a punta (crear conexión → explorar → ejecutar → cancelar → copiar resultados). Las incidencias encontradas se apuntan en el ROADMAP del hito; el hito no se cierra con bloqueos de accesibilidad.

**Grid de resultados:** es el componente de mayor riesgo, porque Qt Quick no implementa `QAccessibleTableInterface` (ARCHITECTURE §13). Su spike en H1.7 incluye la prueba con los tres lectores antes de dar el subhito por cerrado.
