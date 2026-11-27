# Entorno de desarrollo

La versión de Qt se fija **solo** en [`.qt-version`](../.qt-version). La leen `scripts/setup.*`, `cmake/QtLocate.cmake` (y, a través de él, los presets) y la CI.

## Requisitos

| Herramienta | Windows | macOS | Linux |
|---|---|---|---|
| Compilador C++20 | MSVC 2022 Build Tools (carga VCTools) | Xcode Command Line Tools (clang) | gcc ≥ 11 o clang ≥ 14 |
| CMake | ≥ 3.24 | ≥ 3.24 | ≥ 3.24 |
| Ninja, Git, Python 3 | winget | Homebrew | gestor de paquetes |
| Qt | `.qt-version`, `win64_msvc2022_64` en `C:\Qt` | `clang_64` en `~/Qt` | `linux_gcc_64` / `linux_gcc_arm64` en `~/Qt` |
| vcpkg | `C:\vcpkg` | `~/vcpkg` | `~/vcpkg` |

Qt se instala con [aqtinstall](https://github.com/miurahr/aqtinstall), fijado en `scripts/requirements-aqt.txt`. Módulos: el paquete base (qtbase, qtdeclarative/Qt Quick, Quick Controls) más `qtshadertools`.

> **Por qué aqt está fijado a un commit de master:** aqt 3.3.0 (última versión en PyPI) no instala Qt ≥ 6.11 en Windows. Qt reorganizó el repositorio por arquitectura (`qt6_6120/qt6_6120_msvc2022_64/`) y aqt 3.3.0 da el error `Failed to download checksum for the file 'Updates.xml'`. El arreglo (miurahr/aqtinstall#959, PR #1000) está en master sin publicar. Cuando salga una versión > 3.3.0, cambia la línea por `aqtinstall==<versión>`.

## Instalación

### Windows
```powershell
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1          # interactivo
powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 -NonInteractive -SkipCompiler   # CI
```
Pasos manuales:
- La instalación de Build Tools pide **UAC** (admin).
- El script pregunta antes de definir `VCPKG_ROOT` como variable de usuario.
- Ninja + MSVC necesitan el entorno de desarrollador. Antes de compilar, abre **Developer PowerShell for VS 2022** o ejecuta:
  ```powershell
  & 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\Launch-VsDevShell.ps1' -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
  ```

### macOS / Linux
```bash
scripts/setup.sh                    # interactivo; pide confirmación antes de sudo
scripts/setup.sh --non-interactive  # CI
```
- aqt se instala en un venv propio (`~/.cache/quarry/aqt-venv`) para evitar el error PEP 668 `externally-managed-environment`.
- Si la versión de CMake de la distribución es < 3.24, el script instala CMake desde PyPI en `~/.local/bin`.
- El script no modifica tu perfil de shell. Añade tú `export VCPKG_ROOT=~/vcpkg`.

## Compilar y testear
```bash
cmake --preset windows-debug        # o linux-debug / macos-debug / *-release
cmake --build --preset windows-debug
ctest --preset windows-debug
```
Los binarios quedan en `build/<preset>/` y los plugins de providers en `build/<preset>/plugins/quarry/`. Los tests `env-check-smoke` y `quarry-smoke` arrancan las apps en modo offscreen y fallan si aparece algún warning de QML. Añade `-DQUARRY_WARNINGS_AS_ERRORS=ON` para compilar como la CI.

### Dependencias (vcpkg)
- `vcpkg.json` (modo manifiesto) fija las dependencias y el `builtin-baseline`. El primer `cmake --preset` las compila en `build/vcpkg_installed/`, compartido por todos los presets. En Windows tarda unos minutos; después se reutiliza la caché binaria de vcpkg (`%LOCALAPPDATA%\vcpkg\archives` o `~/.cache/vcpkg/archives`).
- Los presets usan triplets propios (`x64-windows-quarry`, `x64-linux-quarry`, `arm64-osx-quarry`) que están en `vcpkg-overlays/triplets/`. Los ports `qtbase` y `qttools` de `vcpkg-overlays/ports/` son *shims* vacíos: así QtKeychain y KSyntaxHighlighting se compilan contra la Qt de aqt (`QT_ROOT_DIR`, que exporta `cmake/QtLocate.cmake`) y vcpkg no compila una segunda Qt. Detalles en ARCHITECTURE.md §9.
- Para añadir una dependencia: inclúyela en `vcpkg.json` (requiere aprobación, ver CLAUDE.md). Si enlaza Qt, añade su nombre a la regex `PORT MATCHES` de los triplets.

Para ejecutar a mano en Windows, Qt tiene que estar en el PATH (o hay que desplegar con `windeployqt`):
```powershell
$env:Path = "C:\Qt\6.12.0\msvc2022_64\bin;$env:Path"; .\build\windows-debug\env-check.exe
```

## Variables de entorno

| Variable | Uso |
|---|---|
| `VCPKG_ROOT` | Obligatoria. Los presets la usan para el toolchain de vcpkg. |
| `QT_ROOT_DIR` | Opcional. Si existe, prevalece sobre `QT_BASE_DIR/<versión>/<arch>`. La define `install-qt-action` en la CI. |
| `QT_BASE_DIR` | Variable de caché de CMake (`-DQT_BASE_DIR=...`) para instalar Qt fuera de la ruta por defecto. |
| `QT_QPA_PLATFORM=offscreen` | Para ejecutar la app en Linux sin pantalla. El test smoke ya la usa. |
| `QUARRY_TEST_POSTGRES_URL` | Opcional. URL libpq (`postgresql://user:pass@localhost:5433/quarry_test`) de un servidor de pruebas desechable; sin ella, la conformidad de PostgreSQL se salta. En local: binarios en zip de PostgreSQL 17 en `%TEMP%`, `initdb` + `pg_ctl start` en un puerto libre. |
| `QUARRY_TEST_SCREEN=<n>` | Opcional. Abre las ventanas de `tst_controls` y `bench_editor` en la pantalla `n` (índice de `QGuiApplication::screens()`), para que no tapen la principal. Sin definir o fuera de rango, no cambia nada. |

## Traducciones
Los textos del código van en inglés (`qsTr`/`tr`). Para actualizar `src/app/i18n/quarry_es.ts` después de cambiar textos: `cmake --build --preset windows-debug --target update_translations` (o el preset de tu sistema). Después edita el `.ts` con Qt Linguist o a mano. Los `.qm` se generan en el build y no se versionan.

## Problemas comunes
- **Windows con MSVC en otro idioma:** los presets fijan `VSLANG=1033`. Sin eso, `/showIncludes` sale traducido ("Nota: inclusión del archivo:"), ninja no detecta las dependencias de las cabeceras y quedan objetos obsoletos. Si compilas fuera de los presets, define `VSLANG=1033`.

- **`python3` abre la Microsoft Store (Windows):** es un alias de `WindowsApps`. Usa `py` o `python`, o desactívalo en *Configuración → Aplicaciones → Alias de ejecución de aplicaciones*.
- **`cmake`/`ninja` no se encuentran justo después de instalarlos:** winget actualiza el PATH del registro, no el de la shell abierta. Abre una terminal nueva. `setup.ps1` ya refresca el PATH por su cuenta.
- **`Qt 6.x.y not found at ...`:** ejecuta el script de setup, o revisa `QT_BASE_DIR` / `QT_ROOT_DIR`.
- **`Could not find a configuration file for package "Qt6" that is compatible with requested version`:** la Qt encontrada no coincide con `.qt-version`. Borra `build/<preset>` y reconfigura.
- **Rutas largas con vcpkg (Windows):** vcpkg está en `C:\vcpkg` para no pasar de MAX_PATH. Si aun así falla, activa `LongPathsEnabled` (requiere admin): `New-ItemProperty HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem -Name LongPathsEnabled -Value 1 -PropertyType DWORD -Force`.
- **Linux: `libQt6DBus.so.6: cannot open shared object file` al ejecutar `quarry` desde el build:** QtKeychain de vcpkg no encuentra el DBus de Qt. Ejecuta con `LD_LIBRARY_PATH=~/Qt/$(cat .qt-version)/gcc_64/lib ./build/linux-debug/quarry`. Se resuelve al empaquetar (H10).
- **Linux: `Could not load the Qt platform plugin "xcb"`:** falta `libxcb-cursor0` u otra `libxcb-*`. Ejecuta de nuevo `scripts/setup.sh` o ejecuta con `QT_DEBUG_PLUGINS=1` para ver qué biblioteca falta.
- **Linux sin pantalla (CI, SSH):** `QT_QPA_PLATFORM=offscreen`.
- **Warning `Qt6QmlAssetDownloaderPrivate could not be found because dependency Qt6TaskTree could not be found`:** es benigno. Es un paquete privado opcional de Qt 6.12 que depende del addon `qttasktree`, que no instalamos a propósito para mantener los módulos de Qt al mínimo.
- **`"vswhere.exe" no se reconoce...` al ejecutar `Launch-VsDevShell.ps1`:** es benigno. Lo imprime un script interno de VS; el entorno queda configurado igual (compruébalo con `cl`).

## CI

Dos niveles de CI, para no esperar una hora por cada arreglo sin perder pruebas. Los decide `scripts/ci-plan.sh` en el trabajo `plan` del workflow.

| Nivel | Qué corre | Cuándo |
|---|---|---|
| **Rápido** (por defecto) | Solo Linux: `clang-format`, compilación en Release con `-Werror` (GCC), tests, `clang-tidy` sobre lo cambiado y la comprobación de instalación | Cualquier push a `main` que no active el completo |
| **Rápido con servidores** | Lo anterior más PostgreSQL, MariaDB, MySQL 8.4, SQL Server, unixODBC y `sshd` | Si el push toca `src/providers/**`, `tests/providers/**`, `src/core/api/**` o el código de túneles SSH y `known_hosts` |
| **Completo** | Windows + Linux con todos los servidores (macOS está en pausa, ver abajo) | Al cerrar un hito (antes de pedir la aprobación), en pull requests, con "Run workflow" en Actions, con `[full-ci]` en el mensaje del commit o el título del PR, y si cambia `CMakeLists.txt`, `CMakePresets.json`, `cmake/`, `vcpkg.json`, `vcpkg-overlays/`, `packaging/`, `.qt-version`, el workflow o `scripts/setup.*` |

- **Documentación:** un push o PR que solo toca `docs/**` o `*.md` no ejecuta la CI. Si el push mezcla docs y código, sí.
- **Sin servidores, sus tests se saltan** (`QSKIP`), igual que en local: no hay variables `QUARRY_TEST_*_URL`.
- **macOS en pausa (desde 2026-11-24):** el nivel completo no construye macOS (ni arm64 ni x64) ni monta el `.dmg` universal ni lo prueba en Intel, para centrar el esfuerzo en Windows y Linux. Motivo inmediato: el `.dmg` pasó del presupuesto de 60 MB (`PERFORMANCE.md`) con QUA-23 y se prefiere decidir eso más adelante. Para incluirlo: "Run workflow" con la casilla **macos**, o `[macos-ci]` en el mensaje del commit o el título del PR. Para reactivarlo siempre, cambiar el valor por defecto de `macos` en `scripts/ci-plan.sh` (`macos=${5:-false}`) y en el `inputs.macos` del workflow. **Qué se pierde:** una regresión solo de macOS, el presupuesto del `.dmg` y la prueba de instalación del `.app` no se ven hasta reactivarlo; los instaladores de macOS de H10 quedan sin comprobar mientras tanto.
- **`clang-tidy` por diff:** solo analiza los `.cpp` cambiados desde el push anterior y los que incluyen un `.h` cambiado (`scripts/ci-tidy-files.sh`). Es completo con "Run workflow" marcando `full`, con `[full-ci]`, y de forma automática si cambia la configuración de compilación o de análisis, no hay commit base o cambian más de 40 fuentes. La compilación y los tests del nivel que corra siempre son completos.
- **`ccache`** en Linux y macOS (cache por commit, restaurada del último run). No se usa en Windows.
- **`tst_controls`** (la suite QML, que mide tiempos de la interfaz en un runner compartido) se reintenta una vez si falla: una carrera de temporización pasa, un fallo real falla dos veces. Los presupuestos de rendimiento de esos tests llevan el +25 % de la CI que fija PERFORMANCE.md. Si falla, el job sube `tst_controls.log` como artefacto.
- **Antes de subir:** `scripts/prepush.ps1` (Windows) o `scripts/prepush.sh` (Linux/macOS) ejecuta lo que más runs ha costado: `clang-format` 18 sobre todo el repo, compilación, `ctest` sin `tst_controls` y `clang-tidy` sobre los ficheros del diff con `origin/main`. Opciones: `-SkipTests`, `-SkipTidy`, `-Preset`, `-Base`. El `clang-tidy` de Windows no ve todo lo que ve el de Linux, y GCC y Clang con `-Werror` solo se prueban en la CI.
- **Un push por paso lógico**, no por arreglo: se acumulan los cambios y se sube una vez, tras el script.
- **Fallos de los tests QML en local:** `tst_controls` cuelga bajo `ctest` en algunas máquinas; se ejecuta archivo a archivo con `QT_QPA_PLATFORM=offscreen` (en Windows, también `QT_QPA_FONTDIR=C:\Windows\Fonts`): `tst_controls.exe -input <ruta>.qml`.
- **Qué se pierde, dicho claro:** una regresión que solo afecte a Windows o a macOS en un commit intermedio no se ve hasta el siguiente nivel completo. Los desencadenantes de la tabla y la CI completa al cerrar cada hito lo acotan.

## Instaladores de Windows (H10)

Se construyen desde el árbol que deja `cmake --install` (ver ARCHITECTURE §15) con `packaging/windows/build_installers.ps1 -TreeDir <árbol> [-TestSign]`. La CI lo hace en el nivel completo de Windows y sube los artefactos (3 días): `quarry-windows-installer` (Inno Setup, sin firmar), `quarry-windows-msix` (MSIX firmado con un certificado de prueba temporal, más `Quarry-test.cer`) y `quarry-windows-tree` (el árbol en zip).

- **Inno Setup 6.3 o posterior** para el instalador (`ISCC.exe`): `winget install JRSoftware.InnoSetup` o el instalador oficial con `/CURRENTUSER`. `makeappx` y `signtool` vienen con el SDK de Windows.
- **Instalar el MSIX de prueba:** confía en `Quarry-test.cer` como administrador (`Import-Certificate -FilePath Quarry-test.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`) e instala el `.msix` (doble clic o `Add-AppxPackage`). Quita el certificado de "Personas de confianza" cuando termines. No es una firma real.
- **Datos de usuario:** el instalador de Inno usa `%APPDATA%\Quarry`; el MSIX redirige las escrituras en `%APPDATA%` a una carpeta por paquete, así que sus conexiones y su `known_hosts` no son los mismos. Desinstalar no borra los datos.

## Paquete de macOS (H10)

La CI (nivel completo) construye `Quarry.app` en `macos-15` (arm64) y en `macos-15-intel` (x64), los une en un `.app` universal y lo empaqueta en `Quarry-<versión>-universal.dmg`, subido como artefacto de 3 días (`quarry-macos-dmg`). Los scripts están en `packaging/macos/`:

- `make_app.sh <directorio-de-compilación> <salida> <versión> <kit-Qt> <lib-vcpkg>` ensambla el `.app` desde el árbol de compilación; `make_universal.sh <arm64.app> <x64.app> <salida.app>` los une con `lipo`; `make_dmg.sh <Quarry.app> <salida.dmg>` hace el `.dmg`. Hay que ejecutarlos en un Mac.
- **Firma:** ad hoc (`codesign -s -`), que Apple Silicon exige para arrancar. No está notarizado, así que Gatekeeper lo bloquea: arrástralo a Aplicaciones y ábrelo con clic derecho → Abrir, o quita la cuarentena con `xattr -dr com.apple.quarantine /Applications/Quarry.app`.
- **Firma real y notarización:** `packaging/macos/sign_and_notarize.sh` (Developer ID, runtime endurecido, `notarytool` y `stapler`) está escrito pero no se ha ejecutado nunca y no está enlazado en la CI. Necesita un certificado Developer ID y un perfil de `notarytool` (`APPLE_SIGN_IDENTITY`, `APPLE_NOTARY_PROFILE`); es probable que haya que ajustar `entitlements.plist` la primera vez.
- **Datos de usuario:** `~/Library/Application Support/Quarry`; las credenciales, en el llavero de macOS.

## Paquetes de Linux (H10)

La CI (nivel completo) construye `Quarry-<versión>-x86_64.AppImage` en el job de Linux y `Quarry-<versión>.flatpak` en `linux-flatpak`, ambos artefactos de 3 días. Los scripts están en `packaging/linux/` y hay que ejecutarlos en Linux (Ubuntu 24.04 es la referencia: el AppImage necesita una glibc igual o más reciente que la suya).

- **AppImage:** `chmod +x Quarry-*.AppImage && ./Quarry-*.AppImage`. Sin FUSE: `./Quarry-*.AppImage --appimage-extract-and-run`. `build_appimage.sh <compilación> <salida> <versión> <kit-Qt> <lib-vcpkg>` lo construye; descarga `linuxdeploy`, su plugin de Qt y `appimagetool` con la versión y el SHA-256 fijados en el propio script. Si ODBC no encuentra drivers, `ODBCSYSINI` apunta a `/etc` por defecto (puedes definir la tuya).
- **Flatpak local:** `flatpak install --user Quarry-*.flatpak` (necesita el runtime: `flatpak install flathub org.freedesktop.Platform//25.08`) y `flatpak run dev.quarry.Quarry`. Tiene acceso de lectura y escritura a tu carpeta personal, red, el agente SSH, el Secret Service y pantalla; **no** a otros discos: para abrir bases de datos fuera de `$HOME`, `flatpak override --user --filesystem=/ruta dev.quarry.Quarry`.
- **Datos de usuario:** el AppImage usa `~/.local/share/Quarry`; el Flatpak, `~/.var/app/dev.quarry.Quarry/data/Quarry`. Las credenciales van al Secret Service.

## Releases y actualizaciones (QUA-30)

Por ahora solo hay releases de **Windows** (`Quarry-<versión>-x64-setup.exe`) y **Linux** (`Quarry-<versión>-x86_64.AppImage`). **Los nombres son un contrato**: la app elige el paquete de su plataforma por el sufijo (`-setup.exe`, `.AppImage`).

- **Publicar:** (1) sube `project(quarry VERSION x.y.z)` en `CMakeLists.txt` y fusiona en `main`; (2) pasa la CI completa en ese commit (`[full-ci]` o "Run workflow"); (3) Actions → **Release** → Run workflow con el id de esa ejecución. `release.yml` solo es manual, no compila nada: descarga los artefactos de esa ejecución (3 días de vida), comprueba que la ejecución es verde y del mismo commit, y crea un **borrador** `v<versión>`. Se revisa y se pulsa "Publish release" a mano; la etiqueta se crea al publicar. Solo una release publicada y que no sea pre-release se ofrece a los usuarios.
- **Qué hace la app:** unos 5 s después del primer fotograma pregunta a `https://api.github.com/repos/Hewcan/quarry/releases/latest` (sin credenciales, en segundo plano); si hay una versión mayor, muestra el diálogo con **Update now** / **Later**. **Help → Check for updates** hace lo mismo a demanda y muestra siempre el resultado. "Update now" descarga el paquete, comprueba su SHA-256 (el `digest` que GitHub añade a cada asset) y pide confirmar para instalar: en Windows lanza el instalador y cierra Quarry; en Linux sustituye el AppImage en ejecución (variable `APPIMAGE`) y lo reabre; si no se ejecuta como AppImage, lo guarda en una carpeta temporal y lo lanza.
- **Sin comprobación:** la variable `QUARRY_NO_UPDATE_CHECK` desactiva la del arranque; `--smoke`, `--bench-startup` y los tests no la hacen (no tienen versión de aplicación o no esperan 5 s).
- **Límites:** macOS y Flatpak no tienen paquete en la release (el diálogo enlaza a la página de la versión; Flatpak se actualizaría por su propio canal); los paquetes no están firmados (el instalador de Windows mostrará SmartScreen); el SHA-256 protege de una descarga corrupta pero la confianza descansa en GitHub y TLS, no en una firma propia.
