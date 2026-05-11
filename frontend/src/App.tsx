import React, { useState, useCallback, useRef, useEffect } from 'react';
import Header from './components/Header';
import Dashboard from './components/Dashboard';
import { useWebSocket } from './hooks/useWebSocket';
import { useSmoothAngles } from './hooks/useSmoothAngles';
import type { Landmark, Angles, AppStatus, RecordedFrame } from './types';

const INITIAL_ANGLES: Angles = [0, 0, 0, 0, 0, 0];

const App: React.FC = () => {
  const ws = useWebSocket();

  const [status, setStatus] = useState<AppStatus>('idle');
  const [landmarks, setLandmarks] = useState<Landmark[] | null>(null);
  const [smoothedLandmarks, setSmoothedLandmarks] = useState<Landmark[] | null>(null);
  const [angles, setAngles] = useState<Angles>(INITIAL_ANGLES);
  const smoothAngles = useSmoothAngles(angles);
  const [isRecording, setIsRecording] = useState(false);
  const [isPlaying, setIsPlaying] = useState(false);
  const [wristAngle, setWristAngle] = useState<number | null>(null);

  // Recording state
  const recordedFramesRef = useRef<RecordedFrame[]>([]);
  const playbackTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const playbackIndexRef = useRef(0);
  const playbackFramesRef = useRef<RecordedFrame[]>([]);

  // Keep last landmarks for recording
  const lastLandmarksRef = useRef<Landmark[] | null>(null);
  const lastAnglesRef = useRef<Angles>(INITIAL_ANGLES);

  // Auto-connect WebSocket on mount
  useEffect(() => {
    ws.connect();
    return () => ws.disconnect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Update angles from WebSocket
  useEffect(() => {
    if (ws.angles.some(v => v !== 0)) {
      setAngles(ws.angles);
      lastAnglesRef.current = ws.angles;
    }
  }, [ws.angles]);

  // Smooth landmarks with EMA as they arrive
  useEffect(() => {
    if (landmarks && landmarks.length === 21) {
      setSmoothedLandmarks(prev => {
        if (!prev) return landmarks;
        const alpha = 0.25;
        return landmarks.map((l, i) => ({
          x: prev[i].x * (1 - alpha) + l.x * alpha,
          y: prev[i].y * (1 - alpha) + l.y * alpha,
          z: prev[i].z * (1 - alpha) + l.z * alpha,
        }));
      });
    }
  }, [landmarks]);

  const handleLandmarks = useCallback((lm: Landmark[], wristAngle?: number | null, handedness?: string) => {
    if (isPlaying) return;

    setLandmarks(lm);
    lastLandmarksRef.current = lm;

    if (wristAngle !== undefined && wristAngle !== null) {
      setWristAngle(wristAngle);
    }

    if (status === 'running') {
      ws.sendLandmarks(lm, wristAngle, handedness);
    }
  }, [status, ws, isPlaying]);

  // Recording logic
  useEffect(() => {
    if (!isRecording) return;
    const interval = setInterval(() => {
      if (lastLandmarksRef.current && lastAnglesRef.current.some(v => v > 0)) {
        recordedFramesRef.current.push({
          landmarks: lastLandmarksRef.current,
          angles: [...lastAnglesRef.current] as Angles,
          timestamp: Date.now(),
        });
      }
    }, 50); // 20fps

    return () => clearInterval(interval);
  }, [isRecording]);

  const handleStart = useCallback(() => {
    ws.connect();       // Reconectar WebSocket si está desconectado
    setStatus('running');
  }, [ws]);

  const handleStop = useCallback(() => {
    setStatus('idle');
    setAngles(INITIAL_ANGLES);
    // No desconectar WebSocket — mantenerlo vivo para cuando vuelva a Start
  }, []);

  const handleRecord = useCallback(() => {
    if (isRecording) {
      // Stop recording & download
      setIsRecording(false);
      const data = recordedFramesRef.current;
      if (data.length > 0) {
        const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `recording-${Date.now()}.json`;
        a.click();
        URL.revokeObjectURL(url);
      }
      recordedFramesRef.current = [];
    } else {
      recordedFramesRef.current = [];
      setIsRecording(true);
    }
  }, [isRecording]);

  const handleReplay = useCallback(() => {
    if (isPlaying) {
      // Stop playback
      setIsPlaying(false);
      if (playbackTimerRef.current) {
        clearTimeout(playbackTimerRef.current);
        playbackTimerRef.current = null;
      }
      return;
    }

    // Prompt to load a recording if none loaded
    if (playbackFramesRef.current.length === 0) {
      const input = document.createElement('input');
      input.type = 'file';
      input.accept = '.json';
      input.onchange = (e: Event) => {
        const file = (e.target as HTMLInputElement).files?.[0];
        if (!file) return;
        const reader = new FileReader();
        reader.onload = (evt) => {
          try {
            const data = JSON.parse(evt.target?.result as string) as RecordedFrame[];
            if (Array.isArray(data) && data.length > 0) {
              playbackFramesRef.current = data;
              startPlayback();
            }
          } catch (err) {
            console.error('Failed to parse recording:', err);
          }
        };
        reader.readAsText(file);
      };
      input.click();
    } else {
      startPlayback();
    }
  }, [isPlaying]);

  function startPlayback() {
    const frames = playbackFramesRef.current;
    if (frames.length === 0) return;

    setIsPlaying(true);
    playbackIndexRef.current = 1;  // Start from frame 1
    const firstTimestamp = frames[0].timestamp;

    function step() {
      if (!playbackFramesRef.current.length) return;

      const idx = playbackIndexRef.current;
      if (idx >= playbackFramesRef.current.length) {
        setIsPlaying(false);
        return;
      }

      const frame = playbackFramesRef.current[idx];
      setLandmarks(frame.landmarks);
      setAngles(frame.angles);

      playbackIndexRef.current = idx + 1;

      // Calcular delay real basado en timestamps
      const currentTimestamp = frame.timestamp;
      const nextIdx = idx + 1;
      let delay = 50; // default
      if (nextIdx < playbackFramesRef.current.length) {
        const nextFrame = playbackFramesRef.current[nextIdx];
        delay = Math.max(16, Math.min(200, nextFrame.timestamp - currentTimestamp));
      }

      playbackTimerRef.current = setTimeout(step, delay);
    }

    // Mostrar primer frame inmediatamente
    const firstFrame = frames[0];
    setLandmarks(firstFrame.landmarks);
    setAngles(firstFrame.angles);

    // Programar el resto
    playbackTimerRef.current = setTimeout(step, 50);
  }

  const handleLoadRecording = useCallback((frames: RecordedFrame[]) => {
    playbackFramesRef.current = frames;
  }, []);

  // Listen for download request from ReplayControls
  useEffect(() => {
    const handleExport = () => {
      const data = recordedFramesRef.current;
      if (data.length > 0) {
        const blob = new Blob([JSON.stringify(data, null, 2)], { type: 'application/json' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `recording-${Date.now()}.json`;
        a.click();
        URL.revokeObjectURL(url);
      }
    };

    const handleRequestDownload = () => {
      const data = recordedFramesRef.current;
      if (data.length > 0) {
        handleExport();
      } else {
        alert('No hay grabación para descargar. Graba una secuencia primero.');
      }
    };

    window.addEventListener('export-recording', handleExport);
    window.addEventListener('request-recording-download', handleRequestDownload);
    return () => {
      window.removeEventListener('export-recording', handleExport);
      window.removeEventListener('request-recording-download', handleRequestDownload);
    };
  }, []);

  return (
    <div className="app">
      <Header
        status={status}
        connected={ws.connected}
        latency={ws.latency}
        isRecording={isRecording}
        isPlaying={isPlaying}
        onStart={handleStart}
        onStop={handleStop}
        onRecord={handleRecord}
        onReplay={handleReplay}
      />
      <Dashboard
        landmarks={landmarks}
        angles={smoothAngles}
        isRecording={isRecording}
        isPlaying={isPlaying}
        smoothedLandmarks={smoothedLandmarks}
        onLandmarks={handleLandmarks}
        onRecord={handleRecord}
        onReplay={handleReplay}
        onLoadRecording={handleLoadRecording}
        cameraActive={status === 'running'}
      />
    </div>
  );
};

export default App;
