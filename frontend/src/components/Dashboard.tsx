import React from 'react';
import CameraView from './CameraView';
import Hands3D from './Hands3D';
import ServoPanel from './ServoPanel';
import AngleChart from './AngleChart';
import ReplayControls from './ReplayControls';
import type { Angles, Landmark } from '../types';

interface DashboardProps {
  landmarks: Landmark[] | null;
  angles: Angles;
  isRecording: boolean;
  isPlaying: boolean;
  smoothedLandmarks: Landmark[] | null;
  onLandmarks: (landmarks: Landmark[]) => void;
  onRecord: () => void;
  onReplay: () => void;
  onLoadRecording: (frames: { landmarks: Landmark[]; angles: Angles; timestamp: number }[]) => void;
  cameraActive: boolean;
}

const Dashboard: React.FC<DashboardProps> = ({
  landmarks,
  angles,
  isRecording,
  isPlaying,
  smoothedLandmarks,
  onLandmarks,
  onRecord,
  onReplay,
  onLoadRecording,
  cameraActive,
}) => {
  return (
    <main className="dashboard">
      <div className="dashboard-left">
        <div className="panel camera-panel">
          <div className="panel-header">
            <h2>Cámara</h2>
          </div>
          <CameraView onLandmarks={onLandmarks} active={cameraActive} />
        </div>
        <div className="panel hand-3d-panel">
          <div className="panel-header">
            <h2>Mano 3D</h2>
          </div>
          <Hands3D landmarks={smoothedLandmarks} />
        </div>
      </div>

      <div className="dashboard-right">
        <div className="panel servo-panel">
          <div className="panel-header">
            <h2>Servos</h2>
          </div>
          <ServoPanel angles={angles} />
        </div>
        <div className="panel chart-panel">
          <div className="panel-header">
            <h2>Ángulos en Tiempo Real</h2>
          </div>
          <AngleChart currentAngles={angles} />
        </div>
        <div className="panel replay-panel">
          <div className="panel-header">
            <h2>Control de Grabación</h2>
          </div>
          <ReplayControls
            isRecording={isRecording}
            isPlaying={isPlaying}
            landmarks={landmarks}
            angles={angles}
            onRecord={onRecord}
            onReplay={onReplay}
            onLoadRecording={onLoadRecording}
          />
        </div>
      </div>
    </main>
  );
};

export default Dashboard;
