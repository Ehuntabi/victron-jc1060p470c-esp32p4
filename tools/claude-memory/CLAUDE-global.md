# Global CLAUDE.md (~/.claude/CLAUDE.md)

Instrucciones que aplican a TODAS las sesiones de Claude Code de este usuario,
independientemente del proyecto.

## Regla 1 — Aplicar siempre las Karpathy guidelines

Antes de cualquier trabajo de código no trivial (write, edit, refactor, review)
invocar la skill **`andrej-karpathy-skills:karpathy-guidelines`** o aplicar sus
4 principios de memoria:

1. **Think Before Coding** — surface assumptions, push back when warranted, ask
   when ambiguous. Don't pick silently between multiple interpretations.
2. **Simplicity First** — minimum code that solves the problem. No speculative
   features, abstractions, or "flexibility" that wasn't requested.
3. **Surgical Changes** — touch only what you must. Don't refactor adjacent
   code. Every changed line must trace to the user's request.
4. **Goal-Driven Execution** — define verifiable success criteria before
   acting. For multi-step tasks: state plan with checks per step.

## Regla 2 — Preguntar TODAS las dudas

Ante cualquier ambigüedad, decisión de diseño, elección de defaults o supuesto,
preguntar al usuario con `AskUserQuestion` antes de actuar. Agrupar dudas en
una sola llamada (hasta 4 preguntas). NO decidir por defecto en silencio,
incluso en cambios pequeños. Aplica también a prompts pasados a sub-agentes.
