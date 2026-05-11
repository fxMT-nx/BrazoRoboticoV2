import React from 'react';
import type { AppStatus } from '../types';

interface HeaderProps {
  status: AppStatus;
  connected: boolean;
  latency: number;
  isRecording: boolean;
  isPlaying: boolean;
  onStart: () => void;
  onStop: () => void;
  onRecord: () => void;
  onReplay: () => void;
}

const Header: React.FC<HeaderProps> = ({
  status,
  connected,
  latency,
  isRecording,
  isPlaying,
  onStart,
  onStop,
  onRecord,
  onReplay,
}) => {
  const statusColor = !connected ? '#ff5252' : status === 'running' ? '#00e676' : '#ffab00';
  const statusText = !connected
    ? 'Desconectado'
    : status === 'running'
      ? 'Transmitiendo'
      : 'En espera';

  return (
    <header className="header">
      <div className="header-left">
        <div className="header-logo">
          <svg width="32" height="32" viewBox="0 0 32 32" fill="none">
            <rect x="4" y="20" width="6" height="8" rx="1" fill="#00e676" opacity="0.8" />
            <rect x="13" y="16" width="6" height="12" rx="1" fill="#00e676" opacity="0.6" />
            <rect x="22" y="18" width="6" height="10" rx="1" fill="#00e676" opacity="0.4" />
            <circle cx="16" cy="6" r="4" fill="#00e676" />
          </svg>
          <span className="header-title">Brazo Robótico V2</span>
        </div>
        <div className="header-status">
          <span
            className="status-dot"
            style={{ backgroundColor: statusColor }}
          />
          <span className="status-text">{statusText}</span>
          {connected && (
            <span className="status-latency">
              {latency.toFixed(0)}ms
            </span>
          )}
        </div>
      </div>

      <div className="header-actions">
        {status !== 'running' ? (
          <button className="btn btn-start" onClick={onStart} disabled={!connected}>
            ▶ Iniciar
          </button>
        ) : (
          <button className="btn btn-stop" onClick={onStop}>
            ■ Detener
          </button>
        )}
        <button
          className={`btn ${isRecording ? 'btn-recording' : 'btn-record'}`}
          onClick={onRecord}
          disabled={isPlaying}
        >
          {isRecording ? '● Grabando' : '◉ Grabar'}
        </button>
        <button
          className="btn btn-replay"
          onClick={onReplay}
          disabled={isRecording}
        >
          {isPlaying ? '↻ Reproduciendo' : '↺ Reproducir'}
        </button>
      </div>
    </header>
  );
};

export default Header;
