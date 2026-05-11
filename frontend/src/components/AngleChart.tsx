import React, { useRef, useEffect, useState, useCallback } from 'react';
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';
import { FINGER_COLORS, FINGER_NAMES, CHART_DURATION_MS, pwmToDegrees, FINGER_LABELS } from '../constants';
import type { Angles } from '../types';

interface ChartDataPoint {
  time: number;  // timestamp
  thumb: number;
  index: number;
  middle: number;
  ring: number;
  pinky: number;
  wrist: number;
}

interface AngleChartProps {
  currentAngles: Angles;
}

const MAX_POINTS = 600; // 30s / 50ms = 600 points

const AngleChart: React.FC<AngleChartProps> = ({ currentAngles }) => {
  const dataRef = useRef<ChartDataPoint[]>([]);
  const [chartData, setChartData] = useState<ChartDataPoint[]>([]);
  const anglesRef = useRef(currentAngles);
  anglesRef.current = currentAngles;

  const updateChart = useCallback(() => {
    const now = Date.now();
    const cutoff = now - CHART_DURATION_MS;
    const angles = anglesRef.current;

    dataRef.current.push({
      time: now,
      thumb: angles[0],
      index: angles[1],
      middle: angles[2],
      ring: angles[3],
      pinky: angles[4],
      wrist: angles[5],
    });

    // Keep only last CHART_DURATION_MS
    if (dataRef.current.length > MAX_POINTS) {
      dataRef.current = dataRef.current.slice(-MAX_POINTS);
    }

    // Prune old entries by timestamp
    dataRef.current = dataRef.current.filter(d => d.time >= cutoff);

    setChartData([...dataRef.current]);
  }, []);

  // Update at ~20fps for chart
  useEffect(() => {
    const interval = setInterval(updateChart, 50);
    return () => clearInterval(interval);
  }, [updateChart]);

  const formatXAxis = (ts: number) => {
    const secs = Math.round((ts - (dataRef.current[0]?.time ?? ts)) / 1000);
    return `-${secs}s`;
  };

  const CustomTooltip = ({ active, payload, label }: any) => {
    if (!active || !payload) return null;
    const relativeTime = label
      ? `${Math.round((label - (dataRef.current[0]?.time ?? label)) / 1000)}s atrás`
      : '';
    return (
      <div className="chart-tooltip">
        <div className="chart-tooltip-time">{relativeTime}</div>
        {payload.map((entry: any, idx: number) => (
          <div key={idx} style={{ color: entry.color }}>
            {FINGER_LABELS[entry.name as keyof typeof FINGER_LABELS] || entry.name}: {pwmToDegrees(entry.value)}° ({Number(entry.value).toFixed(0)}µs)
          </div>
        ))}
      </div>
    );
  };

  return (
    <div className="angle-chart">
      <ResponsiveContainer width="100%" height={200}>
        <LineChart data={chartData} margin={{ top: 5, right: 5, left: 5, bottom: 5 }}>
          <CartesianGrid strokeDasharray="3 3" stroke="#1a2332" />
          <XAxis
            dataKey="time"
            tickFormatter={formatXAxis}
            stroke="#546e7a"
            tick={{ fontSize: 10, fill: '#546e7a' }}
            interval="preserveStartEnd"
            minTickGap={60}
          />
          <YAxis
            domain={[0, 180]}
            stroke="#546e7a"
            tick={{ fontSize: 10, fill: '#546e7a' }}
            tickFormatter={(v: number) => `${v}°`}
          />
          <Tooltip content={<CustomTooltip />} />
          {FINGER_NAMES.map((name, idx) => (
            <Line
              key={name}
              type="monotone"
              dataKey={name}
              stroke={FINGER_COLORS[idx]}
              strokeWidth={2}
              dot={false}
              isAnimationActive={false}
              name={FINGER_LABELS[name]}
            />
          ))}
        </LineChart>
      </ResponsiveContainer>
    </div>
  );
};

export default AngleChart;
