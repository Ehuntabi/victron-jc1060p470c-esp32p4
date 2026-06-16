# Setup autocaravana — 3 proyectos Victron en portátil Linux Mint

Documentación completa para continuar el desarrollo de los proyectos Victron
desde el portátil mientras estás en la autocaravana durante una semana.

**Tag de referencia (estado a 2026-06-16)**: `pre-autocaravana-2026-06-16` en los 3 repos.
Para volver al estado de salida si rompes algo: `git checkout pre-autocaravana-2026-06-16`.

---

## 0. Resumen ejecutivo

3 proyectos, todos ESP-IDF v5.4.4:

| Proyecto | Hardware | Target | Repo GitHub |
|---|---|---|---|
| `victron` (pantalla 7") | Guition JC1060P470C ESP32-P4 + C6 (SDIO) | esp32p4 | `Ehuntabi/victron-jc1060p470c-esp32p4` |
| `victron_mini` | ESP32-C6 (sensor remoto ESP-NOW) | esp32c6 | `Ehuntabi/victron-mini-c6-esp-now` |
| `pantalla_3.5` | ESP32-S3 (display 3.5") | esp32s3 | `Ehuntabi/victron-display-3.5-esp32-s3` |

---

## 1. Setup inicial en el portátil (una sola vez)

### 1.1 Obtener el script de setup

Tres opciones, en orden de facilidad:

**A) Si tienes git+SSH key de GitHub configurada en el portátil:**
```bash
cd ~
git clone git@github.com:Ehuntabi/victron-jc1060p470c-esp32p4.git /tmp/victron-bootstrap
bash /tmp/victron-bootstrap/tools/setup-autocaravana.sh
```

**B) Si NO tienes SSH key de GitHub:**
```bash
cd ~
git clone https://github.com/Ehuntabi/victron-jc1060p470c-esp32p4.git /tmp/victron-bootstrap
bash /tmp/victron-bootstrap/tools/setup-autocaravana.sh
```

**C) Si no tienes acceso a internet** (autocaravana sin wifi):
- Copia `setup-autocaravana.sh` por USB al portátil antes de salir
- Ejecuta `bash /ruta/al/script.sh`
- En este caso necesitas también clonar los 3 repos con USB antes de salir (mira sección 6)

El script hace, sin sudo:
- Verifica dependencias (git, python3, cmake, etc.)
- Comprueba grupo `dialout` (necesario para acceso serie)
- Clona los 3 repos en `~/joint/`
- Instala ESP-IDF 5.4.4 en `~/.espressif/esp-idf-5.4/` para targets `esp32p4,esp32c6,esp32s3`
- Añade aliases útiles a `~/.bashrc`

Si algo falla sin sudo, el script te lo dice claro y sigue con lo que puede.

### 1.2 Tras el script

```bash
source ~/.bashrc        # cargar aliases nuevos
```

Aliases creados:
- `get_idf` → carga el entorno ESP-IDF en la shell actual
- `victron` → cd al proyecto + get_idf
- `victron_mini` → cd al mini + get_idf
- `pantalla` → cd a pantalla_3.5 + get_idf

---

## 2. Comandos diarios

### Workflow típico de cada proyecto:

```bash
victron               # alias: cd ~/joint/victron + carga ESP-IDF
idf.py build          # compilar
idf.py -p /dev/ttyACM0 flash monitor   # flash + serial monitor
# Ctrl+] para salir del monitor
```

**Importante puerto serie**:
- ESP32-P4 (victron): suele aparecer como `/dev/ttyACM0` o `/dev/ttyACM1`
- ESP32-C6/S3 (mini, pantalla): suele aparecer como `/dev/ttyUSB0` (USB-Serial CP210x)
- Para detectar: `ls /dev/ttyUSB* /dev/ttyACM*` antes y después de conectar el cable

### Flujo de trabajo: hago un cambio → flasheo → pruebo

```bash
victron
# editar código
idf.py flash monitor   # rebuild incremental + flash + monitor en uno
```

### Si pierdes el entorno (terminal nueva):

```bash
get_idf      # alias que ejecuta . ~/.espressif/esp-idf-5.4/export.sh
```

---

## 3. Estado actual de cada proyecto (qué probar primero)

### 3.1 `victron` (pantalla 7" ESP32-P4) — el principal

**Hardware en autocaravana**: pantalla DSI 1024x600, conectada al sistema RS-485 NE185.

**Estado al 28-may-2026**:
- ✅ Luces interior/exterior/bomba **se encienden** vía RS-485 (test 28-may 7:42)
- ❌ Luces **no se apagan** tras encenderse (probable `FF 01 00 C0 C0` es "set ON" no toggle)
- ⚠️ Tanks de agua: NO aparecen (frame15 degradado con header `F8 E0`)
- ✅ 230V shore: detectado (bit 4 de b[10] confirmado)
- ❌ Orion Tr 12/12-30 DC/DC: NO llega al P4 (parser idéntico al de pantalla 3.5 que SÍ recibe)

**Lo primero que hay que hacer en la autocaravana**:

1. **Sniff del CHECK button del NE187** (la pista más prometedora):
   - Conectar el panel NE187 al bus RS-485
   - Abrir serial monitor del P4 (`idf.py monitor`)
   - Pulsar CHECK en el NE187
   - Buscar en el log diferencias en `frame15`/`frame20` antes y después
   - Hipótesis: CHECK trigger frame20 canónico con tanques de agua

2. **Probar "set OFF" para luces**: en `main/ne185/ne185.c`, probar comando `FF 01 00 80 80` (set OFF) en vez de toggle. Si funciona → diferenciar ON/OFF explícito.

3. **Debug Orion Tr** — verificar en este orden ANTES de tocar código:
   - MAC del Orion en NVS (via web UI ESP32)
   - AES key correcta (de VictronConnect app)
   - Bluetooth Orion activado (verificar con VictronConnect)
   - Distancia a la caja metálica (probar acercando ESP32)
   - Firmware Orion actualizado
   - Si los 5 OK y aún no llega → mirar logs `[DIAG]` (victron_ble.c líneas 231-263)
   - Hipótesis fuerte: problema esp_hosted SDIO en P4+C6, NO el parser

### 3.2 `victron_mini` (ESP32-C6 sensor remoto)

Estado limpio, sin pendientes obvios. Funciona como sensor ESP-NOW.

### 3.3 `pantalla_3.5` (ESP32-S3)

Sin pendientes activos según memoria.

### Pendientes UI del repo `victron`:

1. Debug rotación del salvapantallas (modo Rotar Live/Frigo/Batería cada N min)
2. Aplicar cards a vistas Live: battery_monitor, solar_charger, simple, simple_devices
3. Victron Keys 2 columnas (intento alternativo, el anterior no renderizaba textos)

---

## 4. Tags importantes (git checkpoints)

```bash
git tag --list 'sdkconfig-known-good*'      # estado SDK conocido (no romper)
git tag --list 'pre-autocaravana*'           # estado al salir de viaje (hoy)
```

| Tag | Cuándo |
|---|---|
| `sdkconfig-known-good-2026-05-27` | sdkconfig validado (no tocar capabilities sin diff) |
| `pre-autocaravana-2026-06-16` | Estado del código al salir de viaje |

### Rollback si algo se rompe en autocaravana:
```bash
git checkout pre-autocaravana-2026-06-16    # vuelve al estado de salida
# o solo el sdkconfig:
git checkout sdkconfig-known-good-2026-05-27 -- sdkconfig
idf.py reconfigure
idf.py build
```

### Después del viaje:
- Crear nuevo tag `post-autocaravana-2026-06-XX` cuando vuelvas
- Si encontraste código estable nuevo: tag `sdkconfig-known-good-2026-06-XX`

---

## 5. Recovery / troubleshooting comunes

### 5.1 Build falla por capabilities ausentes (DSI roto)

**Síntoma**: error sobre `MIPI-DSI` o capabilities en el build.

**Causa probable**: tocaste sdkconfig sin querer (ej. ejecutaste `fullclean`).

**Fix**:
```bash
git checkout sdkconfig-known-good-2026-05-27 -- sdkconfig
idf.py reconfigure
idf.py build
```

**Recordatorio**: SIEMPRE `git diff sdkconfig` antes de aceptar cambios automáticos del menuconfig.

### 5.2 Flash falla con "Permission denied" en /dev/ttyUSB0

```bash
groups   # verifica si esta 'dialout'
# Si no esta: sudo usermod -aG dialout $USER && logout
# (necesita password de sudo)
```

### 5.3 Monitor se cuelga (INT WDT timeout / Guru Meditation)

Posiblemente migración i2c legacy pendiente. Antes el bus i2c con `driver/i2c.h` viejo causaba INT WDT en `i2c_isr_handler_default`. Si ves esto:
- Mirar mensaje exacto, buscar en log "old driver" warnings
- Migrar a `driver/i2c_master.h` siguiendo skill esp-migrate-i2c-legacy

### 5.4 "Expected X found Y" warnings del toolchain

**Ignóralos si el build termina OK**. Memoria explícita: warnings ≠ errors. NO ejecutes `fullclean` para "arreglar" estos warnings, eso fue lo que rompió DSI el 25-may-2026.

### 5.5 Orion Tr no llega al P4

Ya cubierto en sección 3.1 punto 3.

### 5.6 NVS reset (volver a configurar MAC/AES keys desde 0)

```bash
idf.py erase-flash    # cuidado, borra TODO incluyendo bootloader
# Luego flash normal:
idf.py flash monitor
# Reconfigurar WiFi/MAC/keys desde la web UI del ESP32
```

---

## 6. Plan B: sin internet en la autocaravana

Si NO tienes wifi durante el viaje:

### 6.1 Antes de salir, en el PC principal:
```bash
# Crear paquete completo offline
mkdir -p /tmp/viaje-autocaravana
cp -r ~/joint/victron /tmp/viaje-autocaravana/
cp -r ~/joint/victron_mini /tmp/viaje-autocaravana/
cp -r ~/joint/victronsolardisplayesp-multi-device_pantalla_3.5 /tmp/viaje-autocaravana/
cp -r ~/.espressif /tmp/viaje-autocaravana/dot-espressif
# (~/.espressif sera 1-2 GB con todo el toolchain)
tar czf /tmp/viaje.tar.gz -C /tmp viaje-autocaravana
# Copiar viaje.tar.gz a USB
```

### 6.2 En el portátil:
```bash
# Descomprimir
tar xzf /media/$USER/USB/viaje.tar.gz -C ~
mv ~/viaje-autocaravana/dot-espressif ~/.espressif
mv ~/viaje-autocaravana/* ~/joint/
. ~/.espressif/esp-idf-5.4/export.sh   # cargar entorno
```

### 6.3 Desarrollo offline:
- Todos los commits van locales hasta volver
- `git push` al volver

---

## 7. Volver al PC principal vía Tailscale

Si necesitas algo del PC principal (logs viejos, configs, etc.) durante el viaje:

```bash
# El portatil ya tiene Tailscale activo (db3-k72f, 100.104.50.31)
tailscale status                           # ver nodos
# El PC principal NO tiene Tailscale instalado, no puedes alcanzarlo directamente
# Pero todo lo de la LAN de casa SI esta accesible via subnet router del Proxmox:
ssh proxmox                                # via Tailscale 100.122.42.61
ssh root@192.168.1.19                      # mismo Proxmox via subnet
# Desde Proxmox puedes ver LXCs (Pi-hole, PBS, Immich), HA OS, etc.
```

Servicios LAN accesibles desde la autocaravana (vía subnet router 192.168.1.0/24):
- `http://192.168.1.23:8123` — Home Assistant
- `http://192.168.1.25/admin` — Pi-hole admin (sección 7b)
- `https://192.168.1.112:8007` — PBS web UI
- `http://192.168.1.11:2283` — Immich
- `ssh root@192.168.1.19` — Proxmox

**Para acceder al PC principal**, opciones:
- Si tienes IP publica/dyndns: SSH directo
- Vía Tailscale (instalar en PC principal antes de salir si lo necesitas):
  ```bash
  # En el PC principal antes de salir:
  curl -fsSL https://tailscale.com/install.sh | sh
  sudo tailscale up
  ```

---

## 7b. Acceder al Pi-hole desde la autocaravana

El Pi-hole (LXC 102) está en `192.168.1.25` dentro de la LAN de casa.
Desde el portátil **vía Tailscale** puedes alcanzarlo porque Proxmox anuncia
la subnet `192.168.1.0/24` (subnet router aprobado).

### 7b.1 Pre-flight — verificar Tailscale + ruta

Desde el portátil:
```bash
tailscale status            # debe mostrar nodos proxmox / db3-k72f / poco-f5-pro
tailscale ping 192.168.1.25 # OK = la ruta a la LAN funciona via Proxmox subnet router
ping -c 2 192.168.1.25      # ping normal (segundo confirm)
```

Si `tailscale ping` falla:
- Verificar que el portatil tiene `--accept-routes` activo:
  ```bash
  tailscale up --accept-routes
  ```
- Verificar que el subnet router del Proxmox sigue activo:
  ```bash
  ssh proxmox tailscale status
  # Buscar "offers subnet routes: 192.168.1.0/24"
  ```
  Si no aparece o no está aprobado, reactivar (necesita usuario admin):
  ```bash
  ssh proxmox sudo tailscale up --advertise-routes=192.168.1.0/24
  # Luego aprobar en https://login.tailscale.com/admin/machines
  ```

### 7b.2 Web admin (lo más común)

URL: **http://192.168.1.25/admin**

- Username: no requiere (Pi-hole v6+ solo password)
- Password: la que pusiste al setup (está en KeePass)
- Si olvidaste la password:
  ```bash
  ssh proxmox 'pct exec 102 -- pihole setpassword NUEVA_PASSWORD'
  ```

Acceso desde Cinnamon: abrir Firefox/Chrome → `http://192.168.1.25/admin`

### 7b.3 SSH directo al LXC Pi-hole

```bash
ssh proxmox                     # primero al Proxmox via Tailscale
pct enter 102                   # te entra en la shell del LXC Pi-hole
# Comandos utiles dentro:
pihole status
pihole -t                       # tail del log de queries en tiempo real
pihole -c                       # estadisticas
pihole -g                       # update gravity (listas bloqueo)
pihole disable 5m               # pausar bloqueo 5 minutos
pihole enable                   # reactivar
```

Para SSH directo al LXC desde el portátil (sin pasar por Proxmox):
```bash
ssh root@192.168.1.25           # si configuraste authorized_keys del root del LXC
```

### 7b.4 Usar el Pi-hole como DNS desde la autocaravana

Si quieres que las queries DNS del portátil pasen por tu Pi-hole de casa (bloqueo de ads activo):

**Opción A — Cambiar DNS solo del portátil temporalmente**:
```bash
# Linux Mint: NetworkManager
nmcli connection modify "WiFi_AUTOCARAVANA" ipv4.dns "192.168.1.25"
nmcli connection modify "WiFi_AUTOCARAVANA" ipv4.ignore-auto-dns yes
nmcli connection up "WiFi_AUTOCARAVANA"
# Verificar:
nslookup google.com 192.168.1.25
```

**Opción B — Usar el Proxmox como exit node de Tailscale**:
Esto envía TODO el tráfico del portátil por casa (no solo DNS).
```bash
tailscale up --exit-node=proxmox
# Para volver a normal:
tailscale up --exit-node=
```
Verifica antes desde https://login.tailscale.com/admin/machines que el Proxmox tiene "Use as exit node" activado.

### 7b.5 Troubleshooting Pi-hole

| Síntoma | Diagnóstico |
|---|---|
| `pihole status` da "Pi-hole blocking is disabled" | `pihole enable` |
| Web admin no carga (timeout) | LXC parado: `ssh proxmox pct start 102` |
| Web admin pide password viejo | `pihole setpassword` con nuevo (ver 7b.2) |
| DNS no resuelve | FTL parado: `pct exec 102 -- systemctl status pihole-FTL` |
| Pi-hole reset IP | Memoria avisa: NUNCA dhcp en Pi-hole. Verificar `pct config 102` y reaplicar IP estatica `pct set 102 --net0 name=eth0,bridge=vmbr0,gw=192.168.1.1,hwaddr=BC:24:11:8A:30:5A,ip=192.168.1.25/24` |

### 7b.6 Comandos rápidos referencia

```bash
# Stats rapidos sin entrar al LXC:
ssh proxmox 'pct exec 102 -- pihole -c -e'

# Forzar update de listas (gravity):
ssh proxmox 'pct exec 102 -- pihole -g'

# Ver ultimas 50 queries bloqueadas:
ssh proxmox 'pct exec 102 -- pihole -t' | head -50

# Restart Pi-hole entero (sin reiniciar LXC):
ssh proxmox 'pct exec 102 -- pihole restartdns'
```

---

## 8. Cómo hacer un commit limpio en autocaravana

### Política de commits (recordatorio de tus memorias):

- **Commit local automático** tras grupo lógico completo (no commits chicos)
- **Push al final del día/sesión** o bajo petición
- NO commits que rompan el build
- Si tocas `sdkconfig`, hacer diff explícito en el mensaje del commit

### Template de commit:
```
tipo(scope): resumen corto (50 chars max)

- bullet 1: que hizo este commit
- bullet 2: por que
- referencias: ver log de hora HH:MM si aplica

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

Tipos: `feat`, `fix`, `refactor`, `docs`, `chore`, `diag` (diagnostic logging), `ui`

---

## 9. Notas hardware/conexiones

### 9.1 Cable serie para flashear
- ESP32-P4 (victron): USB-C directo. Puerto `/dev/ttyACM*` (CDC nativo)
- ESP32-C6 (victron_mini): USB-C directo a través del chip USB-Serial. Puerto `/dev/ttyUSB*` típicamente
- ESP32-S3 (pantalla_3.5): igual que P4, USB-C directo. Puerto `/dev/ttyACM*` o `/dev/ttyUSB*` según hardware

### 9.2 Si el ESP no entra en modo flash automaticamente
- Mantener BOOT pulsado mientras pulsas RESET
- O añadir `--before=default_reset --after=hard_reset` al `idf.py flash`

### 9.3 NE187 panel (físico)
- Debe estar conectado al mismo bus RS-485 que el ESP32-P4
- Polling 60ms, hold 2+ frames, checksum (b5+b9+b14+b15+0xB1)
- IMPORTANTE: NE187 != NE334 (protocolos distintos)

---

## 10. Si algo va realmente mal: vuelta al PC principal

Si encuentras un bug serio que no sabes resolver:

1. **No improvises destructivamente** (no `fullclean`, no `reset --hard`)
2. **Commit local de lo que tengas** (aunque sea WIP) para preservar trabajo
3. **Push** a GitHub para poder ver desde el PC principal vía web
4. **Anota síntomas** en `~/joint/AUTOCARAVANA-NOTAS.md` (crearlo allí mismo)
5. **Tag de estado** roto: `git tag broken-2026-06-XX-descripcion` para no perderlo
6. **Rollback a known-good**: `git checkout sdkconfig-known-good-2026-05-27 -- sdkconfig`

---

## 11. Checklist rápido pre-viaje (hacer hoy desde el PC principal)

- [x] Commit + push 17 commits pendientes en `victron`
- [x] Tag `pre-autocaravana-2026-06-16` en los 3 repos
- [x] Script `tools/setup-autocaravana.sh` creado
- [x] Documento `AUTOCARAVANA-SETUP.md` (este) creado
- [ ] Llevar cable USB-C de calidad (datos, no solo carga)
- [ ] Adaptador USB-A si el portátil no tiene USB-C suficiente
- [ ] Multímetro (debug RS-485)
- [ ] Foto del NE187/NE185 actual y conexiones
- [ ] KeePass sincronizado en el portátil (por si necesitas creds)
- [ ] Tailscale verificado funcionando en el portátil (`tailscale status`)
- [ ] Verificar que tienes datos móviles para tethering si autocaravana no tiene wifi

---

## 12. Contacto / siguiente sesión

Cuando vuelvas al PC principal, retomar con:
```bash
cd ~/joint/victron
git fetch --all --tags
git log --oneline -20             # ver commits del viaje
git tag --list 'broken-*'         # tags de cosas que rompiste
git tag --list 'sdkconfig-known-good-2026-06-*'   # nuevos known-good si los hay
```

Y avisame para que actualice memorias y siguientes pasos.

Buen viaje.
