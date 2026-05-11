import { useRef, useState, useEffect } from 'react';
import type { Angles } from '../types';

const DURATION_MS = 40; // Duración de la interpolación entre valores

/**
 * Hook que toma ángulos raw (con saltos) y devuelve ángulos interpolados suavemente.
 * Cada vez que rawAngles cambia, interpola desde el valor actual mostrado al nuevo
 * usando cubic ease-out durante ~40ms.
 */
export function useSmoothAngles(rawAngles: Angles): Angles {
  const currentDisplayRef = useRef<Angles>([...rawAngles]);
  const [smoothAngles, setSmoothAngles] = useState<Angles>([...rawAngles]);
  const rafRef = useRef<number>(0);
  const prevRawRef = useRef<Angles>([...rawAngles]);

  useEffect(() => {
    const prevRaw = prevRawRef.current;
    prevRawRef.current = [...rawAngles];

    // Si no hubo cambio real, no interpolar
    const hasChanged = rawAngles.some((v, i) => v !== prevRaw[i]);
    if (!hasChanged) return;

    const startValue = [...currentDisplayRef.current] as Angles;
    const endValue = [...rawAngles] as Angles;
    const startTime = performance.now();

    function animate(now: number) {
      const elapsed = now - startTime;
      const t = Math.min(1, elapsed / DURATION_MS);

      // Cubic ease-out: el movimiento se frena hacia el final
      const eased = 1 - Math.pow(1 - t, 3);

      const current = startValue.map((s, i) =>
        Math.round(s + (endValue[i] - s) * eased)
      ) as Angles;

      setSmoothAngles(current);
      currentDisplayRef.current = current;

      if (t < 1) {
        rafRef.current = requestAnimationFrame(animate);
      }
    }

    // Cancelar animación previa si existe y empezar nueva
    cancelAnimationFrame(rafRef.current);
    rafRef.current = requestAnimationFrame(animate);

    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [rawAngles]);

  return smoothAngles;
}
