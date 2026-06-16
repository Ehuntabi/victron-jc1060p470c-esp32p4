# Sesión 2026-06-16 — Resumen para autocaravana

Documento de referencia con TODO lo importante de la sesión post-vacaciones del 16-jun-2026, accesible desde el portátil de la autocaravana via `git pull` en el repo victron.

---

## 1. Lo que se hizo hoy (5 fixes mayores)

### 1.1 PBS healthcheck — alertas falsas resueltas
- **Problema**: 16 días seguidos email `[PBS] ALERTAS - revisar` con `[FAIL] Servicios PBS no estan ambos active (active=0/2)`.
- **Causa**: `/etc/cron.d/pbs-healthcheck` no definía `PATH`. Cron usaba `/usr/bin:/bin` (sin `/usr/sbin/`), así que `pct exec ... systemctl is-active ...` fallaba en silencio porque no encontraba `pct`. Misma causa: `lvs` también está en `/usr/sbin/` → campo `Thin pool` salía vacío.
- **Fix aplicado**: añadir `PATH=/usr/sbin:/usr/bin:/sbin:/bin` al cron file.
- **Backup**: `/root/pbs-healthcheck.cron.bak.20260616` en Proxmox.
- **Verificación**: tras fix, alertas falsas desaparecen, solo persiste el alert REAL `[WARN] Multimedia_DB3 al 82%`.

### 1.2 PBS restore test E2E — backups validados
- Restauración LXC 102 (Pi-hole) desde snapshot `2026-06-16T02:10:32Z` a VMID 199.
- IP cambiada a 192.168.1.199, MAC nueva → sin colisión con Pi-hole real.
- LXC arrancó, hostname `pihole` confirmado, IP correcta.
- Cleanup OK: `pct stop 199 && pct destroy 199`.
- **Tiempo total**: 33 segundos.
- **Pi-hole producción**: sin afectar (FTL active since 24-may).

### 1.3 Grafana sin datos — InfluxDB path bug
- **Problema**: tras vacaciones, todos los dashboards Grafana vacíos. Última escritura en InfluxDB: **2026-06-06 08:12 UTC** (10 días sin datos).
- **Causa raíz**: HA Core se auto-actualizó a 2026.6.0 el 06-jun 10:13 CEST. La integración `influxdb` construía la URL como `http://host:port` + path (`"/"`) + endpoint → `http://host:port//ping` → HTTP 404 → silencio total (ni log de éxito ni de error).
- **Fix aplicado**: editar `/mnt/data/supervisor/homeassistant/.storage/core.config_entries`:
  - `"path":"/"` → `"path":""`
  - `"verify_ssl":true` → `"verify_ssl":false` (cosmético, http no usa SSL)
- **Backup**: `/mnt/data/supervisor/homeassistant/.storage/core.config_entries.bak.20260616-2113` en HA OS.
- **Verificación**: en <60s tras `ha core restart`, writes vuelven (`SELECT * FROM "W" ORDER BY time DESC` muestra timestamp reciente).
- **Hueco**: del 06-jun al 16-jun NO hay datos. Grafana muestra zona vacía.

### 1.4 Tablet Fully Kiosk — pantalla negra
- **Problema**: la tablet S7_EEA (192.168.1.31) mostraba pantalla negra/error, no cargaba dashboard.
- **Causa raíz**: Fully Kiosk tenía `Start URL = http://192.168.1.135:8123/tablet-sala/0`. La IP 192.168.1.135 era el HA viejo (pre-migración Proxmox). HA actual está en 192.168.1.23.
- **Fixes aplicados**:
  1. `setStringSetting startURL=http://192.168.1.23:8123/tablet-sala/0` vía API Fully Kiosk Remote.
  2. `trusted_networks` en configuration.yaml HA: tablet 192.168.1.31 → auto-login como usuario `kiosk` (id `f878fa50cc6e487db79f5c771d6e869a`, ya existía).
  3. Cambio cosmético: `whitelist_external_dirs` → `allowlist_external_dirs` (deprecado en 2026.6.x).
- **Backup**: `/mnt/data/supervisor/homeassistant/configuration.yaml.bak.20260616-2125`.
- **Pendiente menor**: orientación PORTRAIT no rota vía API; el usuario lo cambia manual en Android Settings.

### 1.5 Preparación viaje autocaravana
- 3 repos commit+push+tag `pre-autocaravana-2026-06-16`.
- Script `tools/setup-autocaravana.sh` (sin sudo).
- Doc `AUTOCARAVANA-SETUP.md` (esta guía + sección 7b Pi-hole).

---

## 2. Estado de infraestructura (snapshot 2026-06-16 21:30 CEST)

### 2.1 Nodos principales

| Nodo | IP | Tipo | Estado |
|---|---|---|---|
| PC principal | 192.168.1.10 | Linux Mint | Up |
| Proxmox host | 192.168.1.19 (LAN) / 100.122.42.61 (Tailscale) | PVE 7.0.2-6 | Uptime 24d 6h |
| LXC 100 Immich | 192.168.1.11 | LXC | running |
| VM 101 HA OS | 192.168.1.23 | qemu | running, HA Core 2026.6.1 |
| LXC 102 Pi-hole | 192.168.1.25 | LXC | running, FTL active 24-may |
| LXC 103 PBS | 192.168.1.112 | LXC | running 28-may |
| Tablet S7_EEA | 192.168.1.31 | Android 9 | running, Fully Kiosk Plus 1.60.1 |
| NAS | 192.168.1.110 | CIFS | OK |

### 2.2 Storage Proxmox (sdb 469GB, 82%)

```
pbs-datastore  199 GB  (chunks PBS, dedup 9.85:1, virtual 1.96 TB)
imagenes       164 GB  (sync nightly NAS)
otros          2.4 GB
```

- ETA al 85% (`399 GB`): ~14 días (`2.5 GB/día` actual, probable que se aplane).
- Plan según `PBS-OPERATIONS.md`: 80%→reducir retención, 85%→SSD adicional.
- **Acción actual**: NO tocar retención. Vigilar 1-2 semanas. La integración influxdb dedup hace que añada poco al filesystem.

### 2.3 PBS backups

```
Job pbs-nightly       schedule 04:00 diario   all=1
GC                    schedule sat 02:00      último 13-jun
Verify monthly        schedule 1st 03:30
Offsite rsync NAS     schedule sat 23:00      último 14-jun 186G
Healthcheck           schedule 09:00 diario   (PATH fix aplicado hoy)
```

Retención: `keep-daily=7, keep-weekly=4, keep-monthly=6`. Setup empezó 28-may, retención mensual aún no se ha ejercitado.

### 2.4 Tailscale

```
100.122.42.61   proxmox      Ehuntabi   linux   offers exit node + subnet 192.168.1.0/24
100.104.50.31   db3-k72f     Ehuntabi   linux   portatil autocaravana
100.94.137.126  poco-f5-pro  Ehuntabi   android movil
```

- PC principal NO tiene Tailscale (acceso solo LAN).
- Subnet router 192.168.1.0/24 aprobado → portátil alcanza toda la LAN desde autocaravana.

---

## 3. Comandos útiles (cheatsheet)

### 3.1 Acceso Proxmox

```bash
ssh proxmox                              # via Tailscale (desde portatil)
ssh root@192.168.1.19                    # via subnet (LAN o Tailscale)

# Listar guests:
pct list                                 # LXCs
qm list                                  # VMs

# Lock check antes de tocar (memoria):
date; pct list                           # evitar 02:30 y 05:00
```

### 3.2 Pi-hole (LXC 102)

```bash
ssh proxmox 'pct enter 102'              # shell del LXC
ssh proxmox 'pct exec 102 -- pihole status'
ssh proxmox 'pct exec 102 -- pihole -c'  # estadisticas
ssh proxmox 'pct exec 102 -- pihole setpassword NUEVA_PASS'

# Web admin:
firefox http://192.168.1.25/admin
```

### 3.3 HA OS (VM 101)

```bash
# Acceso al host HA OS via qemu guest agent (no SSH directo, usar Proxmox):
ssh proxmox 'qm guest exec --timeout 30 101 -- /bin/sh -c "COMANDO"'

# Logs de HA Core:
ssh proxmox 'qm guest exec --timeout 30 101 -- /bin/sh -c "docker logs homeassistant --since 1h 2>&1 | tail -30"'

# Logs de un addon:
ssh proxmox 'qm guest exec --timeout 30 101 -- /bin/sh -c "docker logs addon_a0d7b954_influxdb --tail 50"'

# HA Core stop/start (uso solo emergencia):
ssh proxmox 'qm guest exec --timeout 120 101 -- /bin/sh -c "ha core stop"'
ssh proxmox 'qm guest exec --timeout 180 101 -- /bin/sh -c "ha core start"'
ssh proxmox 'qm guest exec --timeout 180 101 -- /bin/sh -c "ha core check"'   # valida config antes de restart
ssh proxmox 'qm guest exec --timeout 180 101 -- /bin/sh -c "ha core restart"'
```

### 3.4 InfluxDB queries (desde portatil via Tailscale)

```bash
# Verificar último datapoint (cambiar credenciales con las tuyas):
curl -s -G --user "USER:PASS" \
  http://192.168.1.23:8086/query --data-urlencode "db=db3ha" \
  --data-urlencode "q=SELECT * FROM \"W\" ORDER BY time DESC LIMIT 1" \
  --data-urlencode "epoch=s"

# Count escrituras últimos 2 minutos:
curl -s -G --user "USER:PASS" \
  http://192.168.1.23:8086/query --data-urlencode "db=db3ha" \
  --data-urlencode "q=SELECT COUNT(*) FROM \"W\" WHERE time > now() - 2m"
```

### 3.5 Tablet Fully Kiosk Remote (192.168.1.31:2323)

```bash
# Info dispositivo:
curl "http://192.168.1.31:2323/?cmd=deviceInfo&password=PASS&type=json"

# Cambiar Start URL:
curl "http://192.168.1.31:2323/?cmd=setStringSetting&key=startURL&value=URL_ENCODED&password=PASS"

# Forzar reload:
curl "http://192.168.1.31:2323/?cmd=loadStartURL&password=PASS"

# Screenshot (PNG):
curl -o /tmp/tablet.png "http://192.168.1.31:2323/?cmd=getScreenshot&password=PASS"

# Stop screensaver, restart app:
curl "http://192.168.1.31:2323/?cmd=stopScreensaver&password=PASS"
curl "http://192.168.1.31:2323/?cmd=restartApp&password=PASS"
```

### 3.6 PBS

```bash
# Status:
ssh proxmox 'pvesm status pbs-local'

# Healthcheck manual:
ssh proxmox '/usr/local/bin/pbs-healthcheck.sh'

# Lista de snapshots:
ssh proxmox 'pvesm list pbs-local'

# Restore test (NO afecta producción):
ssh proxmox 'pct restore 199 pbs-local:backup/ct/102/SNAPSHOT --storage local-lvm'
# Cambiar IP/MAC ANTES de iniciar
ssh proxmox 'pct set 199 --net0 name=eth0,bridge=vmbr0,gw=192.168.1.1,hwaddr=AA:BB:CC:DD:EE:FF,ip=192.168.1.199/24'
ssh proxmox 'pct start 199; sleep 8; pct exec 199 -- /usr/local/bin/pihole status; pct stop 199; pct destroy 199'
```

---

## 4. Credenciales (DÓNDE están, no qué son)

⚠️ **No se escriben passwords aquí** porque este doc está en repo público. Para obtenerlas:

| Servicio | Dónde encontrar la password |
|---|---|
| Proxmox SSH | Clave SSH ED25519 del portátil ya en `authorized_keys` del Proxmox. `ssh proxmox` debería funcionar sin password |
| Proxmox root web UI | KeePass (busca "Proxmox") |
| PBS token | `/root/.pbs-token-secret` en Proxmox (chmod 600). También en `/root/.pbs-token-raw.txt` |
| Pi-hole admin | KeePass (busca "Pi-hole"). Si perdiste: `ssh proxmox 'pct exec 102 -- pihole setpassword NUEVA'` |
| HA admin (web) | KeePass (busca "Home Assistant") |
| InfluxDB | `ssh proxmox 'qm guest exec ... cat /mnt/data/supervisor/homeassistant/.storage/core.config_entries'` → buscar `domain":"influxdb"` |
| Fully Kiosk Remote | Mismo método: `core.config_entries` → buscar `domain":"fully_kiosk"`. O Settings de Fully Kiosk en la tablet |
| WiFi de casa | KeePass o router config |
| GitHub PAT (para clone HTTPS) | Crear en https://github.com/settings/tokens (no se recupera) |

---

## 5. Pendientes del TODO post-vacaciones

✅ Resueltos hoy:
- Restore test E2E
- Healthcheck PBS PATH bug
- Grafana / InfluxDB path bug
- Tablet Fully Kiosk URL

⏳ Pendientes para cuando vuelvas:
- **Regenerar token PBS** (su secret quedó expuesto en chat sesión 28-may)
- **Copiar creds PBS a KeePass** (`/root/.pbs-credentials-summary.txt` en proxmox)
- **Firewall + 2FA PBS** (aplazado)
- **Encriptación AES-256 datastore PBS** (reconsiderar, con backup obligatorio de key)
- **Fix rsync CIFS offsite** (`-rltD --no-perms --no-owner --no-group` en `/usr/local/bin/pbs-offsite-sync.sh`)
- **Vigilar Multimedia_DB3 82%** (esperar 1-2 semanas)
- **Immich**: 340 fotos abril/mayo + purgar 42795 assets fantasma offline
- **Dedup NAS**: borrar 1749 duplicados (9.15 GB) + analizar 10 carpetas más
- **Joint UI**: salvapantallas rotación, cards Live, Victron Keys 2 columnas

⏳ Pendientes Victron (foco de la autocaravana):
- Sniff CHECK button NE187 → entender frame15 canónico
- Luces NE185 se encienden, no se apagan (probar `FF 01 00 80 80`?)
- Orion Tr 12/12-30 DC/DC no llega al P4 (probable bug esp_hosted SDIO)
- Ventilador GPIO21 (esperando cableado)
- DS18B20 físicos (esperando conexión)

---

## 6. Memorias persistentes creadas/actualizadas hoy

En el PC principal en `~/.claude/projects/-home-jc/memory/`:

| Memoria | Tema |
|---|---|
| `feedback_proxmox_path_in_cron_and_pct_exec.md` | Cron Proxmox y `pct exec` no tienen `/usr/sbin/` en PATH |
| `feedback_ha_config_entries_recovery.md` | Editar `.storage/core.config_entries` con HA Core parado |
| `project_ha_influxdb_grafana.md` | Setup HA InfluxDB Grafana, path debe ser `""` |
| `project_ha_tablet_kiosk.md` | Tablet S7 Fully Kiosk + dashboard tablet-sala |
| `project_todo_post_vacaciones.md` (existente) | Actualizado con progreso |

Acceso desde el portátil: estas memorias son **locales al PC principal**, no viajan con el portátil. Para verlas desde la autocaravana, lo más práctico es leer ESTE documento (que las resume).

---

## 7. Si algo se rompe durante el viaje

1. **Servicio HA, Pi-hole, etc. caído**: `ssh proxmox 'pct status <VMID>'` para ver estado. Restart: `pct restart <VMID>` (LXC) o `qm reboot 101` (HA OS).
2. **PBS sin emails OK**: probablemente PC principal apagado. Backups solo ocurren con PC encendido.
3. **Grafana otra vez sin datos**: verifica primero `SELECT * FROM "W" ORDER BY time DESC LIMIT 1` en InfluxDB. Si timestamp viejo, recargar integración influxdb desde HA UI o editar config_entries (path debe ser `""`, ver sección 1.3).
4. **Tablet pantalla negra**: verificar Start URL con API Fully Kiosk Remote (sección 3.5).
5. **Tailscale no conecta**: `tailscale status` y `tailscale up` en portátil.

---

## 8. Última palabra

Este doc está sincronizado en GitHub (commit `git log -1 --format=%H -- SESION-2026-06-16-resumen.md`).
Cuando vuelvas al PC principal, hacer `git pull` para tener las actualizaciones que hicieses desde la autocaravana.

Buen viaje.
