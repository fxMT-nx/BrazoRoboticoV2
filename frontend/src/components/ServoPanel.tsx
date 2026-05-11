import React from 'react';
import { FINGER_COLORS, FINGER_EMOJIS, FINGER_NAMES, FINGER_LABELS, FINGER_OPEN_THRESHOLD_PWM, pwmToDegrees } from '../constants';
import type { Angles, FingerName } from '../types';

interface ServoPanelProps {
  angles: Angles;
}

const ServoPanel: React.FC<ServoPanelProps> = ({ angles }) => {
  // Rango real MG996R: 1000µs = extendido, 2000µs = flexionado
  // Map to percentage
  const toPercent = (val: number): number => {
    const clamped = Math.max(1000, Math.min(2000, val));
    return ((clamped - 1000) / 1000) * 100;
  };

  return (
    <div className="servo-grid">
      {FINGER_NAMES.map((name: FingerName, idx: number) => {
        const pwm = angles[idx] ?? 0;
        const pct = toPercent(pwm);
        const degrees = pwmToDegrees(pwm);
        const isOpen = idx === 5
          ? pwm >= FINGER_OPEN_THRESHOLD_PWM[idx]   // muñeca: PWM alto = "abierto"
          : pwm <= FINGER_OPEN_THRESHOLD_PWM[idx];  // dedos: PWM bajo = "abierto"

        return (
          <div key={name} className={`servo-column ${isOpen ? 'servo-active' : ''}`}>
            <div className="servo-emoji" style={{ color: FINGER_COLORS[idx] }}>
              {FINGER_EMOJIS[idx]}
            </div>
            <div className="servo-name" style={{ color: FINGER_COLORS[idx] }}>
              {FINGER_LABELS[name]}
            </div>
            <div className="servo-value">{degrees}°</div>
            <div className="servo-value-sub">{pwm.toFixed(0)} µs</div>
            <div className="servo-bar-track">
              <div
                className="servo-bar-fill"
                style={{
                  width: `${pct}%`,
                  backgroundColor: FINGER_COLORS[idx],
                  boxShadow: isOpen
                    ? `0 0 8px ${FINGER_COLORS[idx]}`
                    : 'none',
                }}
              />
            </div>
            <div className="servo-status">
              {idx === 5
                ? (degrees < 40 ? 'Flexionado' : degrees > 100 ? 'Extendido' : 'Neutral')
                : (degrees < 40 ? 'Extendido' : degrees > 100 ? 'Flexionado' : 'Neutral')
              }
            </div>
          </div>
        );
      })}
    </div>
  );
};

export default ServoPanel;
