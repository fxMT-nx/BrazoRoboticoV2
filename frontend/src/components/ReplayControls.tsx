import React, { useRef, useCallback } from 'react';
import type { Landmark, Angles, RecordedFrame } from '../types';

interface ReplayControlsProps {
  isRecording: boolean;
  isPlaying: boolean;
  landmarks: Landmark[] | null;
  angles: Angles;
  onRecord: () => void;
  onReplay: () => void;
  onLoadRecording: (frames: RecordedFrame[]) => void;
}

const ReplayControls: React.FC<ReplayControlsProps> = ({
  isRecording,
  isPlaying,
  landmarks,
  angles,
  onRecord,
  onReplay,
  onLoadRecording,
}) => {
  const fileInputRef = useRef<HTMLInputElement>(null);

  const handleExport = useCallback(() => {
    // This would normally be handled by a parent collecting frames
    // For now, we trigger a save via a custom event or data export
    const event = new CustomEvent('export-recording');
    window.dispatchEvent(event);
  }, []);

  const handleImport = useCallback((e: React.ChangeEvent<HTMLInputElement>) => {
    const file = e.target.files?.[0];
    if (!file) return;

    const reader = new FileReader();
    reader.onload = (evt) => {
      try {
        const data = JSON.parse(evt.target?.result as string) as RecordedFrame[];
        if (Array.isArray(data)) {
          onLoadRecording(data);
        }
      } catch (err) {
        console.error('Failed to parse recording file:', err);
      }
    };
    reader.readAsText(file);

    // Reset input so same file can be re-imported
    if (fileInputRef.current) {
      fileInputRef.current.value = '';
    }
  }, [onLoadRecording]);

  const handleDownload = useCallback(() => {
    // Request parent to provide recording data
    const event = new CustomEvent('request-recording-download');
    window.dispatchEvent(event);
  }, []);

  return (
    <div className="replay-controls">
      <div className="replay-buttons">
        <button
          className={`btn ${isRecording ? 'btn-recording' : 'btn-record'}`}
          onClick={onRecord}
          disabled={isPlaying}
        >
          {isRecording ? '⏹ Detener Grabación' : '⏺ Iniciar Grabación'}
        </button>
        <button
          className="btn btn-replay"
          onClick={onReplay}
          disabled={isRecording}
        >
          {isPlaying ? '⏹ Detener Reproducción' : '▶ Reproducir'}
        </button>
      </div>

      <div className="replay-file-actions">
        <button className="btn btn-export" onClick={handleDownload}>
          💾 Descargar Grabación
        </button>
        <label className="btn btn-import">
          📂 Cargar Grabación
          <input
            ref={fileInputRef}
            type="file"
            accept=".json"
            onChange={handleImport}
            style={{ display: 'none' }}
          />
        </label>
      </div>

      <div className="replay-info">
        <p className="replay-hint">
          {isRecording
            ? 'Grabando frames en tiempo real...'
            : isPlaying
              ? 'Reproduciendo grabación...'
              : 'Presiona "Iniciar Grabación" para capturar una secuencia.'}
        </p>
      </div>
    </div>
  );
};

export default ReplayControls;
