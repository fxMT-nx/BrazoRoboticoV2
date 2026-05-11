import React, { useRef, useEffect } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { DRAW_PAIRS, LANDMARK_LABELS } from '../constants';
import type { Landmark } from '../types';

interface Hands3DProps {
  landmarks: Landmark[] | null;
}

/**
 * Normalizes landmarks to a unit cube centered at origin,
 * inverting Y so the hand is upright in the 3D view.
 */
function normalizeLandmarks(landmarks: Landmark[]): { points: THREE.Vector3[]; center: THREE.Vector3 } {
  if (landmarks.length === 0) {
    return { points: [], center: new THREE.Vector3() };
  }

  let minX = Infinity, maxX = -Infinity;
  let minY = Infinity, maxY = -Infinity;
  let minZ = Infinity, maxZ = -Infinity;

  for (const l of landmarks) {
    if (l.x < minX) minX = l.x;
    if (l.x > maxX) maxX = l.x;
    if (l.y < minY) minY = l.y;
    if (l.y > maxY) maxY = l.y;
    if (l.z < minZ) minZ = l.z;
    if (l.z > maxZ) maxZ = l.z;
  }

  const center = new THREE.Vector3(
    (minX + maxX) / 2,
    (minY + maxY) / 2,
    (minZ + maxZ) / 2,
  );

  const range = Math.max(maxX - minX, maxY - minY, maxZ - minZ, 0.001);

  const points = landmarks.map(l => {
    // Invert Y for 3D display, normalize to ~[-0.5, 0.5]
    return new THREE.Vector3(
      (l.x - center.x) / range,
      -(l.y - center.y) / range,
      (l.z - center.z) / range,
    );
  });

  return { points, center };
}

const Hands3D: React.FC<Hands3DProps> = ({ landmarks }) => {
  const mountRef = useRef<HTMLDivElement>(null);
  const sceneRef = useRef<{
    scene: THREE.Scene;
    camera: THREE.PerspectiveCamera;
    renderer: THREE.WebGLRenderer;
    controls: OrbitControls;
    spheres: THREE.Mesh[];
    lines: THREE.LineSegments;
    animFrame: number;
  } | null>(null);

  // Init Three.js scene once
  useEffect(() => {
    const container = mountRef.current;
    if (!container) return;

    const width = container.clientWidth || 400;
    const height = container.clientHeight || 300;

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x0a0e14);

    const camera = new THREE.PerspectiveCamera(50, width / height, 0.1, 10);
    camera.position.set(0, 0, 2.5);
    camera.lookAt(0, 0, 0);

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setSize(width, height);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    container.appendChild(renderer.domElement);

    // Orbit controls for manual interaction
    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controls.dampingFactor = 0.05;
    controls.autoRotate = false;
    controls.enableZoom = true;
    controls.target.set(0, 0, 0);

    // Ambient + directional lights
    const ambient = new THREE.AmbientLight(0x404060);
    scene.add(ambient);

    const dirLight = new THREE.DirectionalLight(0xffffff, 1.5);
    dirLight.position.set(1, 2, 3);
    scene.add(dirLight);

    const fillLight = new THREE.DirectionalLight(0x448aff, 0.5);
    fillLight.position.set(-1, -1, 1);
    scene.add(fillLight);

    // Spheres for landmarks: create 21
    const sphereGeom = new THREE.SphereGeometry(0.04, 12, 12);
    const spheres: THREE.Mesh[] = [];
    for (let i = 0; i < 21; i++) {
      const material = new THREE.MeshStandardMaterial({
        color: 0x00e676,
        emissive: 0x00e676,
        emissiveIntensity: 0.3,
        roughness: 0.3,
        metalness: 0.1,
      });
      const sphere = new THREE.Mesh(sphereGeom, material);
      sphere.visible = false;
      scene.add(sphere);
      spheres.push(sphere);
    }

    // Lines for bones
    const lineGeom = new THREE.BufferGeometry();
    const positions = new Float32Array(DRAW_PAIRS.length * 2 * 3);
    lineGeom.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    const lineMat = new THREE.LineBasicMaterial({
      color: 0x00e676,
      transparent: true,
      opacity: 0.4,
    });
    const lines = new THREE.LineSegments(lineGeom, lineMat);
    lines.visible = false;
    scene.add(lines);

    // Ground grid helper (subtle)
    const gridHelper = new THREE.GridHelper(2, 10, 0x1a2332, 0x1a2332);
    gridHelper.position.y = -0.6;
    scene.add(gridHelper);

    sceneRef.current = {
      scene,
      camera,
      renderer,
      controls,
      spheres,
      lines,
      animFrame: 0,
    };

    // Animation loop
    function animate() {
      if (!sceneRef.current) return;
      controls.update();
      renderer.render(scene, camera);
      sceneRef.current.animFrame = requestAnimationFrame(animate);
    }
    animate();

    // Handle resize
    const onResize = () => {
      if (!container || !sceneRef.current) return;
      const w = container.clientWidth || 400;
      const h = container.clientHeight || 300;
      camera.aspect = w / h;
      camera.updateProjectionMatrix();
      renderer.setSize(w, h);
    };
    window.addEventListener('resize', onResize);

    return () => {
      window.removeEventListener('resize', onResize);
      cancelAnimationFrame(sceneRef.current?.animFrame ?? 0);
      renderer.dispose();
      if (container.contains(renderer.domElement)) {
        container.removeChild(renderer.domElement);
      }
      sceneRef.current = null;
    };
  }, []);

  // Update 3D positions when landmarks change
  useEffect(() => {
    const ref = sceneRef.current;
    if (!ref || !landmarks || landmarks.length < 21) {
      // Hide everything if no landmarks
      ref?.spheres.forEach(s => (s.visible = false));
      if (ref?.lines) ref.lines.visible = false;
      return;
    }

    const { points } = normalizeLandmarks(landmarks);

    // Update spheres
    const sphereColor = new THREE.Color(0x00e676);
    for (let i = 0; i < 21; i++) {
      const sphere = ref.spheres[i];
      sphere.position.copy(points[i]);
      sphere.visible = true;

      // Wrist is slightly different color
      if (i === 0) {
        (sphere.material as THREE.MeshStandardMaterial).color.setHex(0x448aff);
        (sphere.material as THREE.MeshStandardMaterial).emissive.setHex(0x448aff);
      }
    }

    // Update lines
    const posAttr = ref.lines.geometry.attributes.position as THREE.BufferAttribute;
    const pos = posAttr.array as Float32Array;
    for (let k = 0; k < DRAW_PAIRS.length; k++) {
      const [i, j] = DRAW_PAIRS[k];
      if (points[i] && points[j]) {
        pos[k * 6 + 0] = points[i].x;
        pos[k * 6 + 1] = points[i].y;
        pos[k * 6 + 2] = points[i].z;
        pos[k * 6 + 3] = points[j].x;
        pos[k * 6 + 4] = points[j].y;
        pos[k * 6 + 5] = points[j].z;
      }
    }
    posAttr.needsUpdate = true;
    ref.lines.visible = true;

  }, [landmarks]);

  return <div ref={mountRef} className="hands-3d" />;
};

export default Hands3D;
