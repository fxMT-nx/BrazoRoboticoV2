import { useRef, useEffect, useCallback, useState } from 'react';
import { WS_URL, RECONNECT_BASE_MS, RECONNECT_MAX_MS } from '../constants';
import type { Angles, Landmark } from '../types';

export interface WebSocketState {
  connected: boolean;
  latency: number;
  angles: Angles;
}

const INITIAL_ANGLES: Angles = [0, 0, 0, 0, 0, 0];

/**
 * Hook that manages a WebSocket connection to the robot backend.
 * Auto-reconnects with exponential backoff.
 */
export function useWebSocket() {
  const wsRef = useRef<WebSocket | null>(null);
  const retryCountRef = useRef(0);
  const retryTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const landmarksQueueRef = useRef<Landmark[] | null>(null);
  const wristAngleRef = useRef<number | undefined>(undefined);
  const handednessRef = useRef<string | undefined>(undefined);
  const sendTimerRef = useRef<ReturnType<typeof setInterval> | null>(null);
  const [state, setState] = useState<WebSocketState>({
    connected: false,
    latency: 0,
    angles: INITIAL_ANGLES,
  });

  const connect = useCallback(() => {
    if (wsRef.current?.readyState === WebSocket.OPEN) return;

    const ws = new WebSocket(WS_URL);
    wsRef.current = ws;

    ws.onopen = () => {
      retryCountRef.current = 0;
      setState(prev => ({ ...prev, connected: true }));

      // Send landmarks at regular interval
      sendTimerRef.current = setInterval(() => {
        const landmarks = landmarksQueueRef.current;
        const wristAngle = wristAngleRef.current;
        const handedness = handednessRef.current;
        if (landmarks && ws.readyState === WebSocket.OPEN) {
          const msg: Record<string, unknown> = { landmarks };
          if (wristAngle !== undefined && wristAngle !== null) {
            msg.wrist_angle = wristAngle;
          }
          if (handedness) {
            msg.handedness = handedness;
          }
          ws.send(JSON.stringify(msg));
          landmarksQueueRef.current = null;
          wristAngleRef.current = undefined;
          handednessRef.current = undefined;
        }
      }, 50);

      // Handle incoming messages (angles from server)
      ws.onmessage = (e: MessageEvent) => {
        try {
          const data = JSON.parse(e.data);
          if (data.angles) {
            setState(prev => ({
              ...prev,
              angles: data.angles as Angles,
            }));
          }
        } catch {
          // ignore non-JSON messages
        }
      };
    };

    ws.onclose = () => {
      setState(prev => ({ ...prev, connected: false }));
      cleanupTimers();

      // Exponential backoff reconnect
      const delay = Math.min(
        RECONNECT_BASE_MS * Math.pow(2, retryCountRef.current),
        RECONNECT_MAX_MS
      );
      retryCountRef.current += 1;
      retryTimerRef.current = setTimeout(connect, delay);
    };

    ws.onerror = () => {
      ws.close();
    };
  }, []);

  const disconnect = useCallback(() => {
    cleanupTimers();
    if (retryTimerRef.current) {
      clearTimeout(retryTimerRef.current);
      retryTimerRef.current = null;
    }
    wsRef.current?.close();
    wsRef.current = null;
    retryCountRef.current = 0;
    setState({ connected: false, latency: 0, angles: INITIAL_ANGLES });
  }, []);

  const sendLandmarks = useCallback((landmarks: Landmark[], wristAngle?: number | null, handedness?: string) => {
    landmarksQueueRef.current = landmarks;
    wristAngleRef.current = wristAngle !== undefined && wristAngle !== null ? wristAngle : undefined;
    handednessRef.current = handedness;
  }, []);

  const sendCommand = useCallback((command: string) => {
    if (wsRef.current?.readyState === WebSocket.OPEN) {
      wsRef.current.send(JSON.stringify({ command }));
    }
  }, []);

  function cleanupTimers() {
    if (sendTimerRef.current) {
      clearInterval(sendTimerRef.current);
      sendTimerRef.current = null;
    }
  }

  useEffect(() => {
    return () => {
      disconnect();
    };
  }, [disconnect]);

  return { ...state, connect, disconnect, sendLandmarks, sendCommand };
}
