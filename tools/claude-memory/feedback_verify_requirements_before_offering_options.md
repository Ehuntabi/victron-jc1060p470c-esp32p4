---
name: verify-requirements-before-offering-options
description: "Antes de presentar opciones tecnicas via AskUserQuestion, verificar requisitos de CADA opcion (espacio, tiempo, dependencias). No ofrecer opciones sin comprobar que son ejecutables en el entorno actual"
metadata:
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

**Antes de presentar opciones tecnicas al usuario via `AskUserQuestion`, verificar para CADA opcion los requisitos en el entorno real**.

**Why**: 2026-05-28, presentando plan de backup PBS, ofreci 3 modos vzdump al usuario (snapshot/suspend/stop). El usuario eligio `mode=suspend` por "maxima consistencia". Lo lance sin verificar que mode=suspend hace primero un rsync local al `/var/tmp` del host (documentado en docs Proxmox). `/var/tmp` esta en `pve-root` de 94 GB con 85 GB libres. Immich rootfs eran 163 GB. **No cabia**. vzdump fallo "No space left on device", dejo Immich en estado pausado (resumio solo tras error), causo confusion adicional al diagnosticar mal el servicio (busque `immich-server.service` pero el nombre real es `immich-web.service`). Resultado: 10 min perdidos + alerta innecesaria al usuario + retomada con mode=snapshot (que es lo que debi proponer inicialmente, tras leer docs).

El usuario respondio: "sigues siendo impulsivo en tus decisiones no tienes en cuenta todo". Es un patron recurrente: presentar opciones sin haber leido la documentacion / verificado los requisitos concretos de cada una.

**How to apply**:
- ANTES de cada opcion presentada en `AskUserQuestion`, verificar:
  1. **Espacio**: que sitio necesita cada opcion? donde se escribe temporalmente? cabe en el FS destino?
  2. **Tiempo**: cuanto dura realmente con los volumenes actuales (no estimaciones genericas)?
  3. **Dependencias**: que servicios afecta? que tools necesita? que permisos?
  4. **Estado del sistema**: hay locks, jobs concurrentes, snapshots existentes, thin pool overcommit?
  5. **Failure mode**: si falla esta opcion, en que estado deja al sistema?
- Si no puedo verificar uno de los anteriores, NO ofrecer esa opcion como facil. Marcar explicitamente "requiere X GB libres en pve-root, ACTUAL=85 GB, NECESARIO=163 GB -> no viable".
- Para tareas con backup/restore/storage: leer docs de la herramienta (`man vzdump`, `proxmox-backup-manager help`, PBS admin guide) ANTES de presentar opciones.
- Para tareas en LXCs unprivileged: verificar uid mapping (subuid/subgid) y permisos del bind mount ANTES.
- Para tareas con thin pool LVM: verificar overcommit (`lvs`, `vgs`) y autoextend threshold ANTES.

**Pre-flight checklist personal cuando hay >1 opcion tecnica**:
- [ ] He leido las docs/man de la herramienta?
- [ ] He verificado espacio en cada FS implicado?
- [ ] He verificado state actual (lock, jobs, mounts)?
- [ ] He pensado el failure mode de cada opcion?
- [ ] Si ALGUNA opcion no la he verificado: la marco como "no verificada, riesgo X" o la elimino

Relacionado con: [[feedback-destructive-commands-ask-first]] [[feedback-check-lxc-lock-before-action]] [[feedback-document-failures-immediately]]
