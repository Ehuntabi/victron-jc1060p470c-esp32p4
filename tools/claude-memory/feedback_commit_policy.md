---
name: feedback-commit-policy
description: Politica del usuario sobre commits y pushes - cuando hacer commit automatico y cuando push (sin pedir cada vez)
metadata: 
  node_type: memory
  type: feedback
  originSessionId: 64ed8cac-6e51-4a9f-be24-926d3c876304
---

**Commit:** Hacer commit local AUTOMATICO cuando termino un **grupo logico** de cambios (un fix completo, una feature, un refactor de un componente). NO commitear despues de cada edit pequeno (saturaria la historia). NO esperar a peticion explicita.

**Push:** Push **al final del dia/sesion** o cuando el usuario lo pida explicitamente. NO push automatico tras cada commit (permite revisar antes de publicar).

**Why:** El 2026-05-26 el usuario senalo que sin commits intermedios no podemos "volver un paso atras en caso de desastre". La politica conservadora "solo commit bajo peticion" perdia checkpoints utiles en sesiones largas (5+ horas de codigo). El compromiso: commits significativos para tener puntos de retorno, push manual para mantener control sobre lo publicado.

**How to apply:**
- Tras completar un fix/feature: `git add <archivos>` + `git commit -m "..."` con mensaje describiendo el grupo logico
- Mensajes siguiendo estilo del repo (revisar `git log -5` primero)
- NO incluir archivos sospechosos: secrets, binarios grandes, tar/zip, .env (siempre verificar `git status` antes de `git add`)
- NO `git add .` ni `git add -A`: especificar archivos por nombre
- NO push automatico, salvo que usuario lo pida
- Si hay multiples cambios independientes en la sesion -> multiples commits separados (uno por grupo logico)
- Co-Authored-By footer estandar de Claude Code
- Relacionado: [[feedback-destructive-commands-ask-first]] - commit no es destructivo pero --amend o reset si
