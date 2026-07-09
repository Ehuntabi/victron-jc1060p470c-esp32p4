# 📖 Manual de instalación — Joint SPL 145 Control

Guía **desde cero** para grabar el firmware en el display **Guition JC1060P470C_I** (ESP32-P4), aunque no hayas usado nunca una terminal. Tardarás unos 10 minutos.

> Los ficheros de firmware se descargan de la página de **[Releases](https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/releases)** del proyecto.

### Antes de empezar: 4 palabras que van a salir

- **Firmware:** el programa interno que hace funcionar la pantalla (como el "sistema operativo" de la placa). "Instalar el firmware" = meter este programa dentro de la placa.
- **Terminal:** una ventana donde escribes **comandos** (órdenes en texto) y el ordenador los ejecuta. La abriremos en el Paso 3.
- **Comando:** una línea de texto que escribes (o pegas) en la terminal y ejecutas pulsando la tecla **Enter**.
- **Puerto:** el "canal" por el que el ordenador habla con la placa a través del cable USB. En Windows se llama `COM3`, `COM4`…; en Mac/Linux tiene otro nombre. No te preocupes: más abajo se detecta solo.

### Qué vas a necesitar

- La **placa** Guition JC1060P470C_I (ESP32-P4).
- Un **cable USB-C de datos**. Importante: muchos cables son **solo de carga** y no valen; si uno no funciona, prueba otro.
- Un ordenador con **Windows, macOS o Linux**.
- Conexión a internet (solo para instalar los programas la primera vez).

> No hace falta pulsar ningún botón de la placa: entra en modo grabación por sí sola.

---

## Paso 1 — Descargar el firmware

En la página de [Releases](https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4/releases), abre la última versión y baja hasta la sección **Assets** (una lista plegable; si está cerrada, haz clic en **Assets** para abrirla). Descarga el fichero:

**`joint-spl-145-control-v1.0.0-esp32p4-full.bin`**

Se guardará en tu carpeta de **Descargas**. Es un único fichero que ya contiene todo lo necesario.

> Si descargas una versión distinta de la v1.0.0, el nombre del fichero llevará **otro número** — en los comandos de abajo, cambia `v1.0.0` por el que ponga tu fichero.

---

## Paso 2 — Instalar Python

`esptool` (el programa que graba la placa) funciona sobre **Python**, gratuito. Si ya lo tienes, salta al Paso 3.

**🪟 Windows**
1. Entra en **[python.org/downloads](https://www.python.org/downloads/)** y pulsa el botón amarillo *Download Python*.
2. Abre el instalador que se descargó (doble clic).
3. **MUY IMPORTANTE:** en la primera pantalla, marca abajo la casilla **☑ "Add python.exe to PATH"** **antes** de pulsar *Install Now*. *(Esa casilla es lo que permite escribir `python` desde la terminal; si no la marcas, los comandos darán error.)*
4. Espera a que termine y pulsa *Close*.

**🍎 macOS**
- Los Mac modernos suelen traer Python. Si no, ve a **[python.org/downloads](https://www.python.org/downloads/)**, descarga el instalador para macOS y ábrelo (Continuar → Continuar → Instalar).

**🐧 Linux (Debian / Ubuntu)**
- Casi siempre ya viene. Si no, se instala desde la terminal (Paso 3) con esta línea:
  ```bash
  sudo apt update && sudo apt install -y python3 python3-pip
  ```
  *(`sudo` significa "hazlo como administrador"; te pedirá la contraseña de tu usuario — al escribirla no se ve nada en pantalla, es normal, pulsa Enter.)*

---

## Paso 3 — Abrir una terminal

La terminal es la ventana donde escribiremos los comandos. Para abrirla:

- **🪟 Windows:** menú Inicio → escribe **`PowerShell`** → ábrelo.
- **🍎 macOS:** pulsa **⌘+Espacio** (Spotlight) → escribe **`Terminal`** → Enter.
- **🐧 Linux:** busca **`Terminal`** en tus aplicaciones, o pulsa **`Ctrl`+`Alt`+`T`**.

**Cómo escribir un comando:** haz clic dentro de la ventana de la terminal, escribe (o pega) la línea y pulsa **Enter**.

**Cómo pegar en la terminal** (el `Ctrl`+`V` de siempre a veces no va):
- 🪟 Windows (PowerShell/CMD): **clic derecho** dentro de la ventana pega lo copiado.
- 🍎 macOS: **⌘+V**.
- 🐧 Linux: **`Ctrl`+`Shift`+`V`** (o clic derecho → Pegar).

> 💡 Truco: en cada recuadro de comando de esta página, si pasas el ratón por encima aparece un **icono de copiar 📋** a la derecha. Púlsalo y ya lo tienes copiado para pegar.

**Comprueba que Python está bien:** escribe esto y pulsa Enter:
```
python --version
```
Debe responder algo como `Python 3.12.x`. *(En macOS/Linux, si no responde, prueba `python3 --version`.)*

---

## Paso 4 — Instalar esptool

En la misma terminal, escribe (o pega) y pulsa Enter:

```
pip install esptool
```

*(`pip` es el "instalador de programas" que viene con Python. En macOS/Linux usa `pip3 install esptool`.)* Tardará unos segundos.

Comprueba que quedó instalado:
```
python -m esptool version
```
Debe imprimir un número de versión (p. ej. `esptool.py v4.x`).

<details>
<summary>¿Da error? (clic para ver soluciones)</summary>

- **"pip no se reconoce como comando"** → usa `python -m pip install esptool`.
- **Windows: "python no se reconoce"** → no marcaste la casilla *"Add python.exe to PATH"* en el Paso 2. Reinstala Python marcándola.
- **Linux: "externally-managed-environment"** → usa `pipx install esptool` o `pip3 install --user esptool`.
</details>

---

## Paso 5 — Conectar la placa y situarte en la carpeta del fichero

1. Conecta la placa al ordenador con el cable USB-C.
2. Ahora hay que decirle a la terminal que "entre" en la carpeta donde está el `.bin` (normalmente *Descargas*). Eso se hace con el comando **`cd`** (viene de *change directory*, "cambiar de carpeta"):
   - **🪟 Windows:**
     ```
     cd %USERPROFILE%\Downloads
     ```
     *(`%USERPROFILE%` es un atajo que significa "tu carpeta personal", así no tienes que escribir tu nombre de usuario.)*
   - **🍎 macOS / 🐧 Linux:**
     ```
     cd ~/Downloads
     ```
     *(La `~` significa "tu carpeta personal". Si no sabes escribir la `~`, hay un truco más fácil: escribe `cd` y un espacio, y luego **arrastra la carpeta Descargas** desde el explorador de archivos **hasta la terminal y suéltala**: se pega la ruta sola. Luego pulsa Enter.)*

> ¿Cómo sé que estoy en la carpeta correcta? El principio de la línea de la terminal debería terminar en `Downloads` (o `Descargas`).

---

## Paso 6 — Grabar el firmware

Copia y pega **esta única línea** (es larga: cópiala entera con el icono 📋) y pulsa Enter. No necesitas entender qué significa cada parte del comando: **está pensado para copiarlo tal cual.** esptool busca la placa automáticamente.

**🪟 Windows** (PowerShell o CMD)
```
python -m esptool --chip esp32p4 -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 joint-spl-145-control-v1.0.0-esp32p4-full.bin
```

**🍎 macOS / 🐧 Linux**
```
python -m esptool --chip esp32p4 -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 joint-spl-145-control-v1.0.0-esp32p4-full.bin
```

Verás una barra de progreso (`Writing at 0x...`). Cuando aparezca **`Hash of data verified.`** y **`Done`**, ya está grabado. La placa se reinicia sola y aparece la **pantalla de bienvenida** con **"Joint SPL 145 Control"**.

---

## Paso 7 — Comprobar que funciona

En la pantalla táctil de la placa: entra en **Ajustes → Acerca de**. Debe mostrar la versión que has instalado, por ejemplo:

`Version: v1.0.0    Compilado: Jul 9 2026  11:30:09`

¡Listo! 🎉

---

## Si algo falla

- **No encuentra la placa / "no serial port found":** usa **otro cable USB-C de datos** (no de solo carga) u otro puerto USB del ordenador. En **Windows**, si aún no aparece, mira el apartado *"El driver USB en Windows"* aquí abajo.
- **Linux, error de permisos (`Permission denied` sobre el puerto):** añade tu usuario al grupo `dialout` una sola vez:
  ```bash
  sudo usermod -aG dialout $USER
  ```
  Después **reinicia la sesión** (cierra sesión y vuelve a entrar, o reinicia el equipo) para que tenga efecto. *(Alternativa rápida: ejecuta el comando de grabación poniéndole `sudo` delante.)*
- **Encuentra varios puertos y falla:** desconecta otros aparatos USB-serie que tengas enchufados, o dile a mano cuál es el puerto añadiendo **`-p`** al comando (así saltas la detección automática):
  - Windows: `-p COM3` (mira cuál es en *Administrador de dispositivos → Puertos (COM y LPT)*)
  - macOS: `-p /dev/cu.usbmodem1101` (para ver la lista, escribe `ls /dev/cu.usbmodem*`)
  - Linux: `-p /dev/ttyACM0` (para ver la lista, escribe `ls /dev/ttyACM*`)

  Ejemplo con puerto manual en Windows (este comando usa **`COM3`** — es la parte **`-p COM3`**):
  ```
  python -m esptool --chip esp32p4 -p COM3 -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 joint-spl-145-control-v1.0.0-esp32p4-full.bin
  ```
  👉 Si tu placa usa **otro** puerto (por ejemplo `COM5`), cambia solo el número: sustituye **`-p COM3`** por **`-p COM5`**. En macOS/Linux es igual pero con su nombre de puerto (`-p /dev/cu.usbmodem1101`, `-p /dev/ttyACM0`).

---

### El driver USB en Windows

El *driver* (o "controlador") es la pieza que permite a Windows hablar con el puerto de la placa. Esta placa usa el **USB nativo del ESP32-P4** (de tipo **CDC**, que es un estándar), así que **Windows 10 y 11 ya lo traen y lo instalan solos** al enchufarla: **normalmente no tienes que instalar nada.**

Para comprobar que Windows la reconoce:
1. Enchufa la placa por USB.
2. Abre el **Administrador de dispositivos** (menú Inicio → escribe "Administrador de dispositivos" → Enter).
3. Despliega **Puertos (COM y LPT)**. Si ves algo como **"USB Serial Device (COM3)"**, está todo bien (ese `COM3` es el puerto que usarías con `-p`).
4. Si en cambio ves un aparato con un triángulo amarillo ⚠ (dentro de "Otros dispositivos"):
   - Haz **clic derecho** sobre él → **Actualizar controlador** → **Buscar controladores automáticamente**. Windows lo instala solo, sin descargar nada.

En **macOS** y **Linux** el driver ya viene incluido; no se instala nada.

> **Nota:** otras placas ESP32 llevan un chip aparte (CP2102, CH340, FTDI) que **sí** necesita su propio driver. **Esta no** — usa el USB nativo del chip, así que con los pasos de arriba basta.

---

> **Para desarrolladores:** si tienes ESP-IDF v5.4.4 y el código fuente, puedes compilar y grabar directamente con `idf.py -p <PUERTO> flash`. La imagen fusionada del release es para instalar **sin** compilar nada.
