/**
 * craft — vehicle visuals: hull, engine bloom, ground cushion and a ribbon trail.
 *
 * Purely presentational. It reads a Vehicle's transform each frame and never
 * writes back to the simulation, so the sim stays headless-testable.
 */

import * as THREE from 'three';
import { clamp01 } from '../core/mathx.js';

const TRAIL_POINTS = 46;

export class CraftView {
  constructor(vehicle, cfg, { trail = true, ghost = false } = {}) {
    this.vehicle = vehicle;
    this.cfg = cfg;
    this.isGhost = ghost;
    this.group = new THREE.Group();
    this.group.matrixAutoUpdate = true;

    const body = new THREE.Color(vehicle.color);
    const dark = new THREE.Color(0x0b1220);

    this.mats = {};

    // --- hull: a 4-sided dart, flattened -------------------------------------
    const hullGeo = new THREE.CylinderGeometry(0.05, 3.3, 9.6, 4, 1, false);
    hullGeo.rotateX(Math.PI / 2);
    hullGeo.scale(1.05, 0.44, 1);
    hullGeo.translate(0, 0, -1.2);
    this.mats.hull = new THREE.MeshStandardMaterial({
      color: dark, metalness: 0.85, roughness: 0.28, flatShading: true,
      emissive: body, emissiveIntensity: 0.18
    });
    this.hull = new THREE.Mesh(hullGeo, this.mats.hull);
    this.group.add(this.hull);

    // --- spine / canopy -------------------------------------------------------
    const spineGeo = new THREE.CylinderGeometry(0.1, 1.25, 4.4, 4, 1);
    spineGeo.rotateX(Math.PI / 2);
    spineGeo.scale(1, 0.62, 1);
    spineGeo.translate(0, 0.62, -1.9);
    this.mats.spine = new THREE.MeshStandardMaterial({
      color: 0x0e1826, metalness: 0.6, roughness: 0.2, flatShading: true,
      emissive: body, emissiveIntensity: 0.55
    });
    this.group.add(new THREE.Mesh(spineGeo, this.mats.spine));

    // --- wings ----------------------------------------------------------------
    const wingGeo = new THREE.BoxGeometry(10.4, 0.34, 2.6);
    wingGeo.translate(0, 0.28, -4.1);
    this.mats.wing = new THREE.MeshStandardMaterial({
      color: 0x121c2e, metalness: 0.9, roughness: 0.32, flatShading: true,
      emissive: body, emissiveIntensity: 0.28
    });
    this.group.add(new THREE.Mesh(wingGeo, this.mats.wing));

    // Wing-tip lamps read the craft's orientation from a long way off.
    this.mats.tip = new THREE.MeshBasicMaterial({ color: body });
    for (const sx of [-1, 1]) {
      const tip = new THREE.Mesh(new THREE.BoxGeometry(0.7, 0.5, 2.4), this.mats.tip);
      tip.position.set(sx * 5.0, 0.3, -4.1);
      this.group.add(tip);
    }

    // --- nacelles + engine discs ---------------------------------------------
    this.engineDiscs = [];
    for (const sx of [-1, 1]) {
      const nacGeo = new THREE.CylinderGeometry(0.95, 1.15, 5.4, 10);
      nacGeo.rotateX(Math.PI / 2);
      const nac = new THREE.Mesh(nacGeo, this.mats.wing);
      nac.position.set(sx * 2.7, -0.1, -2.2);
      this.group.add(nac);

      const discMat = new THREE.MeshBasicMaterial({
        color: body, transparent: true, opacity: 0.9,
        blending: THREE.AdditiveBlending, depthWrite: false
      });
      const disc = new THREE.Mesh(new THREE.CircleGeometry(0.92, 18), discMat);
      disc.position.set(sx * 2.7, -0.1, -4.95);
      disc.rotation.y = Math.PI;
      this.group.add(disc);
      this.engineDiscs.push(disc);

      const haloMat = new THREE.MeshBasicMaterial({
        color: body, transparent: true, opacity: 0.25,
        blending: THREE.AdditiveBlending, depthWrite: false, side: THREE.DoubleSide
      });
      const halo = new THREE.Mesh(new THREE.CircleGeometry(2.1, 18), haloMat);
      halo.position.copy(disc.position);
      halo.rotation.y = Math.PI;
      this.group.add(halo);
      this.engineDiscs.push(halo);
    }

    // --- ground cushion glow --------------------------------------------------
    this.groundMat = new THREE.MeshBasicMaterial({
      color: body, transparent: true, opacity: 0.22,
      blending: THREE.AdditiveBlending, depthWrite: false
    });
    this.groundGlow = new THREE.Mesh(new THREE.CircleGeometry(7.5, 22), this.groundMat);
    this.groundGlow.rotation.x = -Math.PI / 2;
    this.groundGlow.visible = false;

    if (ghost) {
      for (const m of Object.values(this.mats)) {
        m.transparent = true;
        m.opacity = 0.30;
        m.depthWrite = false;
        if (m.emissiveIntensity !== undefined) m.emissiveIntensity *= 0.7;
      }
      this.mats.tip.transparent = true;
      this.mats.tip.opacity = 0.45;
      this.mats.tip.depthWrite = false;
    }

    // --- ribbon trail ---------------------------------------------------------
    if (trail) this._buildTrail(body);

    this._pos = new THREE.Vector3();
    this._euler = new THREE.Euler();
    this._quat = new THREE.Quaternion();
  }

  _buildTrail(color) {
    const N = TRAIL_POINTS;
    this.history = [];
    const positions = new Float32Array(N * 2 * 3);
    const colors = new Float32Array(N * 2 * 3);
    const indices = [];
    for (let i = 0; i < N - 1; i++) {
      const a = i * 2, b = i * 2 + 1, c = i * 2 + 2, d = i * 2 + 3;
      indices.push(a, b, c, b, d, c);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    g.setAttribute('color', new THREE.BufferAttribute(colors, 3));
    g.setIndex(indices);
    g.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 1e4);
    this.trailMat = new THREE.MeshBasicMaterial({
      vertexColors: true, transparent: true, opacity: 0.85,
      blending: THREE.AdditiveBlending, depthWrite: false, side: THREE.DoubleSide
    });
    this.trail = new THREE.Mesh(g, this.trailMat);
    this.trail.frustumCulled = false;
    this.trailColor = color.clone();
    this.trailIntensity = 0;
  }

  /**
   * @param {number} dt
   * @param {(x:number,z:number)=>number} terrain
   */
  update(dt, terrain, opts = {}) {
    const v = this.vehicle;
    this.group.position.copy(v.position);
    this._euler.set(v.pitch, v.yaw, v.roll, 'YXZ');
    this.group.quaternion.setFromEuler(this._euler);

    // Engine output: thrust + boost + surge.
    const out = Math.min(1.6, 0.35 + v.speedNorm * 0.75 + (v.boosting ? 0.75 : 0) + v.surgeNorm * 0.9);
    for (let i = 0; i < this.engineDiscs.length; i++) {
      const disc = this.engineDiscs[i];
      const isHalo = i % 2 === 1;
      const s = isHalo ? 0.7 + out * 0.9 : 0.85 + out * 0.32;
      disc.scale.setScalar(s);
      // `out` can exceed 1 under boost+surge; scale may grow but opacity may not.
      disc.material.opacity = clamp01(isHalo ? 0.10 + out * 0.24 : 0.55 + out * 0.35);
    }
    this.mats.spine.emissiveIntensity = 0.4 + out * 0.55;
    this.mats.hull.emissiveIntensity = 0.14 + out * 0.22 + v.impactFlash * 0.8;

    if (v.impactFlash > 0.01) {
      this.mats.hull.emissive.setHex(0xff5533).lerp(new THREE.Color(v.color), 1 - v.impactFlash);
    } else {
      this.mats.hull.emissive.setHex(v.color);
    }

    // Ground cushion
    if (this.isGhost) { this.groundGlow.visible = false; }
    const gh = terrain(v.position.x, v.position.z);
    const dy = v.position.y - gh;
    if (!this.isGhost && dy < 60) {
      this.groundGlow.visible = true;
      this.groundGlow.position.set(v.position.x, gh + 0.6, v.position.z);
      const k = 1 - Math.min(1, dy / 60);
      this.groundMat.opacity = 0.05 + k * 0.35;
      this.groundGlow.scale.setScalar(0.7 + k * 0.9);
    } else if (!this.isGhost) {
      this.groundGlow.visible = false;
    }

    if (!this.trail) return;

    // --- trail ribbon ---------------------------------------------------------
    const last = this.history[0];
    if (!last || last.distanceToSquared(v.position) > 1.4) {
      this.history.unshift(v.position.clone());
      if (this.history.length > TRAIL_POINTS) this.history.length = TRAIL_POINTS;
    }
    const H = this.history;
    const pos = this.trail.geometry.attributes.position;
    const col = this.trail.geometry.attributes.color;
    const up = this._up || (this._up = new THREE.Vector3(0, 1, 0));
    const side = this._side || (this._side = new THREE.Vector3());
    const dir = this._dir || (this._dir = new THREE.Vector3());

    const target = 0.35 + v.speedNorm * 0.65 + v.surgeNorm * 0.5;
    this.trailIntensity += (target - this.trailIntensity) * Math.min(1, dt * 8);

    for (let i = 0; i < TRAIL_POINTS; i++) {
      const p = H[Math.min(i, H.length - 1)];
      const q = H[Math.min(i + 1, H.length - 1)] || p;
      dir.subVectors(p, q);
      if (dir.lengthSq() < 1e-6) dir.set(0, 0, 1);
      dir.normalize();
      side.crossVectors(dir, up);
      if (side.lengthSq() < 1e-6) side.set(1, 0, 0);
      side.normalize();

      const t = i / (TRAIL_POINTS - 1);
      const w = (2.6 - t * 2.35) * (0.5 + this.trailIntensity * 0.9);
      const i6 = i * 6;
      pos.array[i6 + 0] = p.x + side.x * w;
      pos.array[i6 + 1] = p.y + side.y * w - t * 0.6;
      pos.array[i6 + 2] = p.z + side.z * w;
      pos.array[i6 + 3] = p.x - side.x * w;
      pos.array[i6 + 4] = p.y - side.y * w - t * 0.6;
      pos.array[i6 + 5] = p.z - side.z * w;

      const a = Math.pow(1 - t, 2.1) * (0.25 + this.trailIntensity);
      col.array[i6 + 0] = this.trailColor.r * a;
      col.array[i6 + 1] = this.trailColor.g * a;
      col.array[i6 + 2] = this.trailColor.b * a;
      col.array[i6 + 3] = this.trailColor.r * a;
      col.array[i6 + 4] = this.trailColor.g * a;
      col.array[i6 + 5] = this.trailColor.b * a;
    }
    pos.needsUpdate = true;
    col.needsUpdate = true;
    this.trailMat.opacity = 0.55 + this.trailIntensity * 0.4;
    void opts;
  }

  dispose() {
    this.group.traverse((o) => {
      if (o.geometry) o.geometry.dispose();
      if (o.material) o.material.dispose();
    });
  }
}
