# ADR-0001: Renombrar "IK Engine" a "PoseMapper"

**Fecha:** 2026-05-10
**Estado:** Aceptado
**Decidido por:** Equipo de desarrollo
**Revisión programada:** 2026-11-10

## Contexto

El proyecto V1 denominaba "IK Engine" al módulo responsable de convertir los 21 landmarks 3D de MediaPipe en señales PWM para los 5 servos MG996R.

En V2, tras analizar la estrategia de control, se determina que **no existe cinemática inversa real** en el sistema:

1. **1 DOF por dedo**: Cada dedo tiene un solo servo MG996R. No hay cadena cinemática multi-DOF que requiera solvers de matrices Jacobianas.
2. **Estrategia simplificada**:
   - **Pulgar (servo 0)**: Comparación del eje X entre `THUMB_TIP` y `THUMB_IP`. Cuando la punta cruza hacia adentro, el dedo se cierra.
   - **Dedos largos (servos 1-4)**: Cálculo del ángulo 3D en la articulación PIP (proximal interphalangeal) entre los vectores `(TIP → PIP)` y `(MCP → PIP)`. Mapeo lineal del ángulo a PWM.
3. **No hay eslabones rígidos, cadenas cinemáticas ni matrices de transformación homogénea**: El mapping es directamente landmarks → PWM.

## Decisión

Renombrar el módulo de "IK Engine" a **PoseMapper** (clase `PoseMapper` en `backend/pose_mapper.py`).

El nombre "PoseMapper" describe exactamente lo que hace: **mapear landmarks de una pose de mano a valores PWM de servo**.

## Alternativas consideradas

| Alternativa | Descartada por |
|-------------|---------------|
| IK Engine | Implica cinemática inversa que no existe. Confunde a futuros desarrolladores que buscarían solvers de matrices Jacobianas. |
| FingerAngleMapper | Más preciso pero más verboso. PoseMapper captura que es un mapeo de pose completa, no solo ángulos. |
| HandToPWM | Demasiado genérico. No deja claro qué tipo de transformación ocurre. |

## Consecuencias

**Positivas:**
- Elimina ambigüedad: futuros desarrolladores no buscarán cinemática inversa donde no existe.
- El nombre describe exactamente la funcionalidad: mapear landmarks → PWM.
- Consistencia con el glosario del dominio: "Pose" y "Mapper" son términos canónicos en V2.

**Negativas / trade-offs:**
- Rompe compatibilidad con documentación V1 (`docs/informe-completo.md`).
- El archivo `pose_mapper.py` ya está implementado; cualquier referencia externa al "IK Engine" debe actualizarse.

## Términos de dominio afectados

- **PoseMapper** (nuevo término canónico) — ver `CONTEXT.md`
- ❌ "IK Engine" (anti-término) — ver `CONTEXT.md`

## Señales de revisión

- Si en el futuro se añaden grados de libertad adicionales por dedo (2+ DOF) que requieran cinemática inversa real, este ADR debe reevaluarse y el módulo podría renombrarse a algo como `KinematicPoseSolver`.
