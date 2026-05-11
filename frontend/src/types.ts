export interface Landmark {
  x: number;
  y: number;
  z: number;
}

export type Angles = [number, number, number, number, number, number];

export type FingerName = 'thumb' | 'index' | 'middle' | 'ring' | 'pinky' | 'wrist';

export interface WsMessage {
  angles: Angles;
  timestamp: number;
}

export interface RecordedFrame {
  landmarks: Landmark[];
  angles: Angles;
  timestamp: number;
}

export type AppStatus = 'idle' | 'running' | 'error';

export interface AppState {
  status: AppStatus;
  isRecording: boolean;
  isPlaying: boolean;
  landmarks: Landmark[] | null;
  angles: Angles;
  latency: number;
}
