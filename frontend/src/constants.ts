import type { FingerName } from './types';

export const WS_URL = `${window.location.protocol === 'https:' ? 'wss' : 'ws'}://${window.location.hostname}${window.location.port ? ':' + window.location.port : ''}/ws`;
export const EMA_ALPHA = 0.25;
export const SEND_INTERVAL_MS = 50;
export const CHART_DURATION_MS = 30000;
export const RECONNECT_BASE_MS = 1000;
export const RECONNECT_MAX_MS = 30000;

export const FINGER_COLORS = ['#ffab00', '#00e676', '#448aff', '#e040fb', '#69f0ae', '#ff6d00'];
export const FINGER_NAMES: FingerName[] = ['thumb', 'index', 'middle', 'ring', 'pinky', 'wrist'];
export const FINGER_EMOJIS = ['👍', '☝️', '🖕', '💍', '🤙', '↕️'];
export const FINGER_OPEN_THRESHOLD_PWM = [1200, 1150, 1150, 1150, 1150, 1200];

/** Labels en español para mostrar en la UI */
export const FINGER_LABELS: Record<FingerName, string> = {
  thumb: 'Pulgar',
  index: 'Índice',
  middle: 'Corazón',
  ring: 'Anular',
  pinky: 'Meñique',
  wrist: 'Muñeca',
};

/** Convertir PWM (500-2500 µs) a grados (0-180°) */
export function pwmToDegrees(pwm: number): number {
  // Rango típico MG996R: 1000µs = 0°, 2000µs = 180°
  const clamped = Math.max(500, Math.min(2500, pwm));
  if (clamped <= 1000) return 0;
  if (clamped >= 2000) return 180;
  return Math.round(((clamped - 1000) / 1000) * 180);
}

/** MediaPipe landmark pairs that form the hand skeleton */
export const DRAW_PAIRS: [number, number][] = [
  [0, 1], [0, 5], [0, 17], [1, 2], [2, 3], [3, 4],
  [5, 6], [6, 7], [7, 8],
  [9, 10], [10, 11], [11, 12],
  [13, 14], [14, 15], [15, 16],
  [17, 18], [18, 19], [19, 20],
  [5, 9], [9, 13], [13, 17],
];

/** Labels for the 21 MediaPipe landmarks */
export const LANDMARK_LABELS: string[] = [
  'Wrist',
  'Thumb-CMC', 'Thumb-MCP', 'Thumb-IP', 'Thumb-TIP',
  'Index-MCP', 'Index-PIP', 'Index-DIP', 'Index-TIP',
  'Middle-MCP', 'Middle-PIP', 'Middle-DIP', 'Middle-TIP',
  'Ring-MCP', 'Ring-PIP', 'Ring-DIP', 'Ring-TIP',
  'Pinky-MCP', 'Pinky-PIP', 'Pinky-DIP', 'Pinky-TIP',
];

/** Landmark indices that are the tips of each finger */
export const FINGER_TIP_INDICES: Record<FingerName, number> = {
  thumb: 4,
  index: 8,
  middle: 12,
  ring: 16,
  pinky: 20,
  wrist: 0,  // WRIST landmark
};
