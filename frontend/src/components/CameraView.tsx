import React, { useRef, useEffect, useCallback, useState } from 'react';
import { FilesetResolver, HandLandmarker } from '@mediapipe/tasks-vision';
import { FINGER_COLORS, FINGER_TIP_INDICES, FINGER_NAMES, FINGER_LABELS, EMA_ALPHA, FINGER_OPEN_THRESHOLD_PWM, DRAW_PAIRS } from '../constants';
import type { Landmark, FingerName } from '../types';

interface CameraViewProps {
  onLandmarks: (landmarks: Landmark[], wristAngle?: number | null, handedness?: string) => void;
  active: boolean;
}

type FingerTipStatus = Record<FingerName, { open: boolean; x: number; y: number }>;

const INITIAL_FINGERS: FingerTipStatus = {
  thumb: { open: false, x: 0, y: 0 },
  index: { open: false, x: 0, y: 0 },
  middle: { open: false, x: 0, y: 0 },
  ring: { open: false, x: 0, y: 0 },
  pinky: { open: false, x: 0, y: 0 },
  wrist: { open: false, x: 0, y: 0 },
};

/** Determine if a finger is "open" based on distance from palm center to tip */
function isFingerOpen(landmarks: Landmark[], finger: FingerName): boolean {
  const tipIdx = FINGER_TIP_INDICES[finger];
  const mcpIdx = finger === 'thumb' ? 2 : (FINGER_TIP_INDICES[finger] - 3);
  if (!landmarks[tipIdx] || !landmarks[mcpIdx]) return false;

  const dx = landmarks[tipIdx].x - landmarks[mcpIdx].x;
  const dy = landmarks[tipIdx].y - landmarks[mcpIdx].y;
  const dz = landmarks[tipIdx].z - landmarks[mcpIdx].z;
  const dist = Math.sqrt(dx * dx + dy * dy + dz * dz);

  // Empirically, ~0.15 is a good threshold for "open" vs "closed"
  return dist > 0.15;
}

const CameraView: React.FC<CameraViewProps> = ({ onLandmarks, active }) => {
  const videoRef = useRef<HTMLVideoElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const handLandmarkerRef = useRef<HandLandmarker | null>(null);
  const animFrameRef = useRef<number>(0);
  const [fingers, setFingers] = useState<FingerTipStatus>(INITIAL_FINGERS);
  const [cameraReady, setCameraReady] = useState(false);
  const [modelReady, setModelReady] = useState(false);
  const smoothedRef = useRef<Landmark[] | null>(null);
  const wristAngleRef = useRef<number | null>(null);
  const offscreenCanvasRef = useRef<HTMLCanvasElement | null>(null);  // ← NUEVO
  const forearmDotRef = useRef<{ x: number; y: number } | null>(null); // ← NUEVO

  // Guardar onLandmarks en una ref para evitar recrear detect
  const onLandmarksRef = useRef<(landmarks: Landmark[], wristAngle?: number | null, handedness?: string) => void>(onLandmarks);
  useEffect(() => {
    onLandmarksRef.current = onLandmarks;
  }, [onLandmarks]);

  // Guardar active en una ref para usarla dentro del bucle detect
  const activeRef = useRef(active);
  useEffect(() => {
    activeRef.current = active;
  }, [active]);

  // Initialize MediaPipe HandLandmarker + PoseLandmarker
  useEffect(() => {
    let cancelled = false;
    async function init() {
      try {
        const fileset = await FilesetResolver.forVisionTasks(
          'https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@0.10.18/wasm'
        );
        if (cancelled) return;

        // HandLandmarker (rápido, cada frame)
        const handLandmarker = await HandLandmarker.createFromOptions(fileset, {
          baseOptions: {
            modelAssetPath: 'https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task',
            delegate: 'GPU',
          },
          runningMode: 'VIDEO',
          numHands: 1,
        });

        if (!cancelled) {
          handLandmarkerRef.current = handLandmarker;
          setModelReady(true);
        }
      } catch (err) {
        console.error('Failed to load models:', err);
      }
    }
    init();
    return () => { cancelled = true; };
  }, []);

  // Start camera
  useEffect(() => {
    let cancelled = false;
    async function startCamera() {
      try {
        const stream = await navigator.mediaDevices.getUserMedia({
          video: { facingMode: 'user', width: { ideal: 640 }, height: { ideal: 480 } },
        });
        if (cancelled || !videoRef.current) return;
        videoRef.current.srcObject = stream;
        await videoRef.current.play();
        setCameraReady(true);
      } catch (err) {
        console.error('Camera access denied:', err);
      }
    }
    startCamera();
    return () => { cancelled = true; };
  }, []);

  // Detection loop — estable, sin dependencias que cambien
  const detect = useCallback(() => {
    // Pausar detección si no está activo (status !== 'running')
    if (!activeRef.current) {
      animFrameRef.current = requestAnimationFrame(detect);
      return;
    }

    const video = videoRef.current;
    const canvas = canvasRef.current;
    const handLandmarker = handLandmarkerRef.current;
    if (!video || !canvas || !handLandmarker || video.readyState < 2) {
      animFrameRef.current = requestAnimationFrame(detect);
      return;
    }

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // ── REDIMENSIONAR CANVAS AL CONTENEDOR (no al video) ──
    const containerW = canvas.clientWidth;
    const containerH = canvas.clientHeight;

    if (containerW === 0 || containerH === 0) {
      animFrameRef.current = requestAnimationFrame(detect);
      return;
    }

    if (canvas.width !== containerW || canvas.height !== containerH) {
      canvas.width = containerW;
      canvas.height = containerH;
    }

    // ── CALCULAR ÁREA VISIBLE DEL VIDEO (object-fit: contain) ──
    const videoW = video.videoWidth;
    const videoH = video.videoHeight;

    const fitScale = Math.min(containerW / videoW, containerH / videoH);
    const drawW = videoW * fitScale;
    const drawH = videoH * fitScale;
    const offsetX = (containerW - drawW) / 2;
    const offsetY = (containerH - drawH) / 2;

    // ── 1. HAND LANDMARKER cada frame ──
    const handResult = handLandmarker.detectForVideo(video, performance.now());
    ctx.clearRect(0, 0, canvas.width, canvas.height);

    // ── DETECTAR PUNTO VERDE EN EL ANTEBRAZO ──
    // Escanear el video en busca de un marcador verde
    // Si se encuentra, se usa como referencia para calcular wrist angle
    const SCAN_W = Math.floor(videoW / 2);  // Era / 4 — mejor resolución
    const SCAN_H = Math.floor(videoH / 2);
    if (video.readyState >= 2) {
      const dot = findGreenDot(video, SCAN_W, SCAN_H);
      if (dot) {
        // Escalar las coordenadas del scan al tamaño del canvas
        forearmDotRef.current = {
          x: offsetX + (dot.x / SCAN_W) * drawW,
          y: offsetY + (dot.y / SCAN_H) * drawH,
        };
      } else {
        forearmDotRef.current = null;
      }
    }

    if (handResult.landmarks && handResult.landmarks.length > 0) {
      const rawLandmarks = handResult.landmarks[0] as Landmark[];

      // EMA smoothing (igual que antes)
      if (!smoothedRef.current) {
        smoothedRef.current = rawLandmarks.map(l => ({ ...l }));
      } else {
        for (let i = 0; i < rawLandmarks.length; i++) {
          smoothedRef.current[i].x =
            smoothedRef.current[i].x * (1 - EMA_ALPHA) + rawLandmarks[i].x * EMA_ALPHA;
          smoothedRef.current[i].y =
            smoothedRef.current[i].y * (1 - EMA_ALPHA) + rawLandmarks[i].y * EMA_ALPHA;
          smoothedRef.current[i].z =
            smoothedRef.current[i].z * (1 - EMA_ALPHA) + rawLandmarks[i].z * EMA_ALPHA;
        }
      }

      const lm = smoothedRef.current;

      // ── CALCULAR ÁNGULO DE MUÑECA ──
      // Prioridad 1: punto verde en antebrazo (referencia real)
      // Prioridad 2: landmarks 2D (fallback)
      let wristAngle: number | null = null;
      if (lm && lm.length >= 21) {
        // Intentar con punto verde primero
        const dot = forearmDotRef.current;
        if (dot) {
          wristAngle = calcWristAngleWithForearm(
            dot, lm, containerW, containerH,
            videoW, videoH, fitScale, offsetX, offsetY
          );
        }

        // Fallback a método 2D si no hay punto verde
        if (wristAngle === null) {
          wristAngle = calcWristFlexion2D(lm);
        }

        // EMA smoothing
        if (wristAngleRef.current !== null && wristAngle !== null) {
          wristAngle = wristAngleRef.current * 0.7 + wristAngle * 0.3;
        }
        wristAngleRef.current = wristAngle;
      }

      // Extraer handedness del resultado de MediaPipe
      let handedness: string | undefined;
      if (handResult.handedness && handResult.handedness.length > 0 && handResult.handedness[0].length > 0) {
        handedness = handResult.handedness[0][0].categoryName; // 'Left' o 'Right'
      }

      onLandmarksRef.current(lm.map(l => ({ ...l })), wristAngle, handedness);

      // ── DIBUJAR EN COORDENADAS DEL VIDEO VISIBLE ──
      const mapX = (lx: number) => offsetX + lx * drawW;
      const mapY = (ly: number) => offsetY + ly * drawH;

      // Bones (líneas)
      ctx.strokeStyle = 'rgba(0, 230, 118, 0.5)';
      ctx.lineWidth = 3;
      for (const [i, j] of DRAW_PAIRS) {
        if (lm[i] && lm[j]) {
          ctx.beginPath();
          ctx.moveTo(mapX(lm[i].x), mapY(lm[i].y));
          ctx.lineTo(mapX(lm[j].x), mapY(lm[j].y));
          ctx.stroke();
        }
      }

      // Landmarks (puntos)
      for (let i = 0; i < lm.length; i++) {
        const x = mapX(lm[i].x);
        const y = mapY(lm[i].y);
        ctx.beginPath();
        const radius = [4, 8, 12, 16, 20].includes(i) ? 6 : 3.5;
        ctx.arc(x, y, radius, 0, 2 * Math.PI);
        ctx.fillStyle = [4, 8, 12, 16, 20].includes(i) ? '#00e676' : 'rgba(0, 230, 118, 0.6)';
        ctx.fill();
      }

      // Finger tip indicators
      const newFingers = { ...INITIAL_FINGERS };
      for (const name of FINGER_NAMES) {
        const tipIdx = FINGER_TIP_INDICES[name];
        if (lm[tipIdx]) {
          newFingers[name] = {
            open: isFingerOpen(lm, name),
            x: mapX(lm[tipIdx].x),
            y: mapY(lm[tipIdx].y),
          };
        }
      }
      setFingers(newFingers);

      // Draw finger tip circles
      FINGER_NAMES.forEach((name, idx) => {
        const f = newFingers[name];
        if (f.x === 0 && f.y === 0) return;
        ctx.beginPath();
        ctx.arc(f.x, f.y, 12, 0, 2 * Math.PI);
        ctx.strokeStyle = FINGER_COLORS[idx];
        ctx.lineWidth = 3;
        ctx.stroke();
        if (f.open) {
          ctx.fillStyle = FINGER_COLORS[idx] + '30';
          ctx.fill();
        }
      });

      // ── DIBUJAR PUNTO DE REFERENCIA (antebrazo) ──
      if (forearmDotRef.current) {
        const dot = forearmDotRef.current;
        ctx.beginPath();
        ctx.arc(dot.x, dot.y, 8, 0, 2 * Math.PI);
        ctx.fillStyle = '#00ff00';
        ctx.fill();
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 2;
        ctx.stroke();
        ctx.fillStyle = '#ffffff';
        ctx.font = '10px monospace';
        ctx.fillText('antebrazo', dot.x + 12, dot.y + 4);
      }
    }

    animFrameRef.current = requestAnimationFrame(detect);
  }, []);  // SIN dependencias, solo se crea una vez

  useEffect(() => {
    if (cameraReady && modelReady) {
      const loop = () => {
        detect();
      };
      animFrameRef.current = requestAnimationFrame(loop);
    }
    return () => {
      if (animFrameRef.current) {
        cancelAnimationFrame(animFrameRef.current);
      }
    };
  }, [cameraReady, modelReady]);  // Sin detect como dependencia

  // Draw finger legend on canvas overlay
  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    const drawLegend = () => {
      const w = canvas.width;
      if (w === 0) return;

      const legendX = 8;
      const legendY = 8;
      const itemHeight = 20;
      const radius = 6;

      FINGER_NAMES.forEach((name, idx) => {
        const y = legendY + idx * itemHeight;
        const f = fingers[name];
        ctx.beginPath();
        ctx.arc(legendX + radius, y + radius, radius, 0, 2 * Math.PI);
        ctx.fillStyle = FINGER_COLORS[idx];
        ctx.globalAlpha = f.open ? 1.0 : 0.3;
        ctx.fill();
        ctx.globalAlpha = 1.0;
        ctx.font = '11px monospace';
        ctx.fillStyle = '#c8d6e5';
        ctx.fillText(FINGER_LABELS[name], legendX + radius * 2 + 8, y + radius + 4);
      });
    };

    // Draw legend on next frame
    const id = requestAnimationFrame(() => {
      if (canvas.width > 0) drawLegend();
    });
    return () => cancelAnimationFrame(id);
  }, [fingers]);

  return (
    <div className="camera-view">
      <video
        ref={videoRef}
        className="camera-video"
        playsInline
        muted
      />
      <canvas
        ref={canvasRef}
        className="camera-overlay"
      />
      {!cameraReady && (
        <div className="camera-loading">
          <div className="spinner" />
          <span>Iniciando cámara...</span>
        </div>
      )}
      {cameraReady && !modelReady && (
        <div className="camera-loading">
          <div className="spinner" />
          <span>Cargando modelo de detección...</span>
        </div>
      )}
    </div>
  );
};

// ── Funciones auxiliares ──

/** Calcula la flexión/extensión de muñeca usando landmarks 2D de imagen.
 *  
 *  En coordenadas de imagen normalizadas [0,1]:
 *  - x: 0=izquierda, 1=derecha
 *  - y: 0=arriba, 1=abajo
 *  
 *  Vector: WRIST(0) → MIDDLE_MCP(9) (más estable que el tip)
 *  atan2(dx, -dy): 0°=mano recta, +flexión, -extensión
 *  
 *  Para mano izquierda, se invierte el signo porque MediaPipe refleja X.
 *  
 *  Returns: grados (rango típico ±60-80°) o null.
 */
function calcWristFlexion2D(landmarks: Landmark[]): number | null {
  const wrist = landmarks[0];
  const ref = landmarks[9]; // MIDDLE_MCP — punto estable
  
  if (!wrist || !ref) return null;
  
  // Vector en coordenadas de imagen
  const dx = ref.x - wrist.x;
  const dy = ref.y - wrist.y;
  
  // Detectar mano izquierda/derecha
  const thumbX = landmarks[4]?.x ?? 0;
  const pinkyX = landmarks[17]?.x ?? 0;
  const isRight = thumbX < pinkyX;
  
  // atan2(dx, -dy): Y de imagen apunta hacia abajo, negamos para que
  // 0° = mano apuntando recto hacia arriba
  let angleDeg = Math.atan2(dx, -dy) * (180 / Math.PI);
  
  // Invertir para mano izquierda (espejo en X)
  if (!isRight) {
    angleDeg = -angleDeg;
  }
  
  // ── GANANCIA: amplificar rango ──
  // atan2(dx, -dy) da solo ~±25° para movimiento típico de muñeca
  // Multiplicamos por 3 para alcanzar ~±75° y llenar el rango del servo
  const GAIN = 3.0;
  angleDeg = angleDeg * GAIN;
  
  // Limitar a ±90° para que no se salga del rango esperado
  return Math.max(-90, Math.min(90, angleDeg));
}

// ── Funciones para detección de punto verde en el antebrazo ──

/** Detecta un punto/píxel verde en el canvas.
 *
 *  Escanea el canvas buscando píxeles con:
 *  - G > 150 (verde alto)
 *  - R < 120 (rojo bajo)
 *  - B < 120 (azul bajo)
 *  - G > R + 50 (verde domina claramente)
 *
 *  Devuelve la posición del centroide o null si no se encuentra.
 */
function findGreenDot(
  video: HTMLVideoElement,
  scanW: number,
  scanH: number
): { x: number; y: number } | null {
  // Crear canvas temporal para leer píxeles del video
  const tempCanvas = document.createElement('canvas');
  tempCanvas.width = scanW;
  tempCanvas.height = scanH;
  const ctx = tempCanvas.getContext('2d');
  if (!ctx) return null;

  // Dibujar el frame actual del video
  ctx.drawImage(video, 0, 0, scanW, scanH);

  // Leer píxeles
  const imageData = ctx.getImageData(0, 0, scanW, scanH);
  const data = imageData.data;

  let sumX = 0, sumY = 0, count = 0;
  const step = 3; // Era 2 — con el doble de resolución, step 3 mantiene rendimiento

  for (let y = 0; y < scanH; y += step) {
    for (let x = 0; x < scanW; x += step) {
      const idx = (y * scanW + x) * 4;
      const r = data[idx];
      const g = data[idx + 1];
      const b = data[idx + 2];

      // Verde: canal G domina claramente sobre R y B
      if (g > 100 && g > r * 1.3 && g > b * 1.3) {
        sumX += x;
        sumY += y;
        count++;
      }
    }
  }

  if (count > 10) {
    return { x: sumX / count, y: sumY / count };
  }
  return null;
}

/** Calcula el ángulo de muñeca usando el punto verde del antebrazo como referencia.
 *
 *  Vector antebrazo: forearmDot → WRIST(0)
 *  Vector palma: WRIST(0) → MIDDLE_MCP(9)
 *  Ángulo 3D entre ambos vectores en el plano de la imagen.
 *
 *  Returns: grados (-90 a +90) o null.
 */
function calcWristAngleWithForearm(
  forearmDot: { x: number; y: number },
  landmarks: Landmark[],
  containerW: number,
  containerH: number,
  videoW: number,
  videoH: number,
  fitScale: number,
  offsetX: number,
  offsetY: number
): number | null {
  const wrist = landmarks[0];
  const midMCP = landmarks[9];
  if (!wrist || !midMCP) return null;

  // Convertir landmarks normalizados a coordenadas de canvas (píxeles)
  const wristX = offsetX + wrist.x * videoW * fitScale;
  const wristY = offsetY + wrist.y * videoH * fitScale;
  const mcpX = offsetX + midMCP.x * videoW * fitScale;
  const mcpY = offsetY + midMCP.y * videoH * fitScale;

  // Vector antebrazo: forearmDot → WRIST (en coordenadas canvas)
  const fdx = wristX - forearmDot.x;
  const fdy = wristY - forearmDot.y;

  // Vector palma: WRIST → MIDDLE_MCP
  const pdx = mcpX - wristX;
  const pdy = mcpY - wristY;

  // Magnitudes
  const magF = Math.sqrt(fdx * fdx + fdy * fdy);
  const magP = Math.sqrt(pdx * pdx + pdy * pdy);
  if (magF < 1 || magP < 1) return null;

  // Ángulo entre vectores (producto punto)
  const dot = fdx * pdx + fdy * pdy;
  const cosA = Math.max(-1, Math.min(1, dot / (magF * magP)));
  const angleDeg = Math.acos(cosA) * (180 / Math.PI);

  // Signo: producto cruz en Z (determina flexión vs extensión)
  const crossZ = fdx * pdy - fdy * pdx;
  const signed = crossZ > 0 ? -angleDeg : angleDeg;

  return Math.max(-90, Math.min(90, signed));
}

export default CameraView;
