/**
 * scene — everything visual: sky, procedural canyon, gate chain, structure and
 * the speed FX that sell velocity.
 */

import * as THREE from 'three';
import { clamp, clamp01 } from '../core/mathx.js';

const SKY_VERT = /* glsl */ `
  varying vec3 vDir;
  void main() {
    vDir = normalize(position);
    gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
  }
`;

const SKY_FRAG = /* glsl */ `
  uniform vec3 uZenith;
  uniform vec3 uHorizon;
  uniform vec3 uGround;
  uniform vec3 uSunDir;
  uniform vec3 uSunCol;
  varying vec3 vDir;
  void main() {
    vec3 d = normalize(vDir);
    float h = d.y;
    vec3 col = mix(uHorizon, uZenith, smoothstep(0.0, 0.55, h));
    col = mix(col, uGround, smoothstep(0.0, -0.35, h));
    float sun = pow(max(dot(d, normalize(uSunDir)), 0.0), 22.0);
    float glow = pow(max(dot(d, normalize(uSunDir)), 0.0), 3.0);
    col += uSunCol * (sun * 1.6 + glow * 0.18);
    // faint horizon band = readable sense of altitude at 400 km/h
    col += vec3(0.05, 0.16, 0.24) * pow(1.0 - abs(h), 12.0);
    gl_FragColor = vec4(col, 1.0);
  }
`;

export class World {
  constructor(scene, course, cfg) {
    this.scene = scene;
    this.course = course;
    this.cfg = cfg;

    // Scratch objects must exist before the builders below use them.
    this._m = new THREE.Matrix4();
    this._q = new THREE.Quaternion();
    this._v = new THREE.Vector3();
    this._s = new THREE.Vector3();
    this._col = new THREE.Color();
    this._t = 0;

    this._buildSky();
    this._buildTerrain();
    this._buildGates();
    this._buildStructure();
    this._buildStreaks();
  }

  // -------------------------------------------------------------------------
  _buildSky() {
    // Density is a gameplay value, not just atmosphere: the gate chain must
    // still read at ~1500 m or you are flying blind into the next ring.
    this.scene.fog = new THREE.FogExp2(0x0a1424, 0.00055);
    this.scene.background = new THREE.Color(0x05070f);

    const skyMat = new THREE.ShaderMaterial({
      vertexShader: SKY_VERT,
      fragmentShader: SKY_FRAG,
      side: THREE.BackSide,
      depthWrite: false,
      fog: false,
      uniforms: {
        uZenith: { value: new THREE.Color(0x04061a) },
        uHorizon: { value: new THREE.Color(0x1d3a63) },
        uGround: { value: new THREE.Color(0x060912) },
        uSunDir: { value: new THREE.Vector3(-0.45, 0.22, -1).normalize() },
        uSunCol: { value: new THREE.Color(0x2fb6ff) }
      }
    });
    const sky = new THREE.Mesh(new THREE.SphereGeometry(9000, 32, 20), skyMat);
    sky.frustumCulled = false;
    sky.renderOrder = -1000;
    this.sky = sky;
    this.scene.add(sky);

    // Stars
    const N = 1800;
    const pos = new Float32Array(N * 3);
    for (let i = 0; i < N; i++) {
      const u = Math.random() * 2 - 1;
      const th = Math.random() * Math.PI * 2;
      const r = Math.sqrt(Math.max(0, 1 - u * u));
      const rad = 7200;
      pos[i * 3] = Math.cos(th) * r * rad;
      pos[i * 3 + 1] = Math.abs(u) * rad * 0.9 + 200;
      pos[i * 3 + 2] = Math.sin(th) * r * rad;
    }
    const sg = new THREE.BufferGeometry();
    sg.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    this.stars = new THREE.Points(sg, new THREE.PointsMaterial({
      color: 0xbfe9ff, size: 26, sizeAttenuation: true,
      transparent: true, opacity: 0.75, depthWrite: false, fog: false
    }));
    this.stars.frustumCulled = false;
    this.scene.add(this.stars);

    // Key + rim lights
    const key = new THREE.DirectionalLight(0x9fd8ff, 1.5);
    key.position.set(-300, 420, -200);
    this.scene.add(key);
    const rim = new THREE.DirectionalLight(0xff4fa8, 0.85);
    rim.position.set(360, 140, 420);
    this.scene.add(rim);
    this.scene.add(new THREE.HemisphereLight(0x33608f, 0x140a22, 0.9));
  }

  // -------------------------------------------------------------------------
  _buildTerrain() {
    const c = this.course;
    const zMin = c.samples[0].z - 400;
    const zMax = c.samples[c.sampleCount - 1].z + 500;

    let xLo = Infinity, xHi = -Infinity;
    for (let i = 0; i < c.czCount; i++) {
      if (c.czX[i] < xLo) xLo = c.czX[i];
      if (c.czX[i] > xHi) xHi = c.czX[i];
    }
    const pad = 1150;
    xLo -= pad; xHi += pad;

    const CX = 420, CZ = 1400, RES = 25;
    this.terrainChunks = [];
    const mat = new THREE.MeshStandardMaterial({
      vertexColors: true, roughness: 0.92, metalness: 0.06, flatShading: true
    });

    const low = new THREE.Color(0x08111d);
    const mid = new THREE.Color(0x151d3a);
    const wall = new THREE.Color(0x2a1745);
    const lip = new THREE.Color(0x1fbfd6);

    const hw = this.cfg.course.halfWidth;

    for (let cz = zMin; cz < zMax; cz += CZ) {
      for (let cx = xLo; cx < xHi; cx += CX) {
        const nx = Math.ceil(CX / RES), nz = Math.ceil(CZ / RES);
        const g = new THREE.PlaneGeometry(CX, CZ, nx, nz);
        g.rotateX(-Math.PI / 2);
        const p = g.attributes.position;
        const colors = new Float32Array(p.count * 3);
        for (let i = 0; i < p.count; i++) {
          const wx = p.getX(i) + cx + CX / 2;
          const wz = p.getZ(i) + cz + CZ / 2;
          const h = c.terrainHeight(wx, wz);
          p.setY(i, h);

          const cl = c.centerlineAtZ(wz);
          const d = Math.abs(wx - cl.x);
          const rel = clamp01(d / (hw * 2.5));
          const band = 1 - Math.min(1, Math.abs(d - hw * 1.0) / (hw * 0.16));
          const mottle = 0.5 + 0.5 * c.noise.fbm2(wx * 0.004, wz * 0.004, 3);

          this._col.copy(low).lerp(mid, rel * 0.8);
          this._col.lerp(wall, clamp01((rel - 0.45) / 0.55) * 0.9);
          this._col.multiplyScalar(0.75 + mottle * 0.5);
          if (band > 0) this._col.lerp(lip, band * 0.85);

          colors[i * 3] = this._col.r;
          colors[i * 3 + 1] = this._col.g;
          colors[i * 3 + 2] = this._col.b;
        }
        g.setAttribute('color', new THREE.BufferAttribute(colors, 3));
        g.computeVertexNormals();

        const mesh = new THREE.Mesh(g, mat);
        mesh.position.set(cx + CX / 2, 0, cz + CZ / 2);
        mesh.matrixAutoUpdate = false;
        mesh.updateMatrix();
        mesh.frustumCulled = true;
        this.scene.add(mesh);
        this.terrainChunks.push(mesh);
      }
    }
    this.terrainMaterial = mat;
  }

  // -------------------------------------------------------------------------
  _buildGates() {
    this.gateViews = [];
    const upZ = new THREE.Vector3(0, 0, 1);

    for (const gate of this.course.gates) {
      const grp = new THREE.Group();
      grp.position.copy(gate.center);
      this._q.setFromUnitVectors(upZ, gate.normal);
      grp.quaternion.copy(this._q);

      const ringMat = new THREE.MeshBasicMaterial({
        color: 0x1d3a55, transparent: true, opacity: 0.55,
        blending: THREE.AdditiveBlending, depthWrite: false
      });
      const ring = new THREE.Mesh(new THREE.TorusGeometry(gate.radius, 0.85, 6, 42), ringMat);
      grp.add(ring);

      const discMat = new THREE.MeshBasicMaterial({
        color: 0x0e2a3c, transparent: true, opacity: 0.13,
        blending: THREE.AdditiveBlending, depthWrite: false, side: THREE.DoubleSide
      });
      const disc = new THREE.Mesh(new THREE.CircleGeometry(gate.radius * 0.98, 34), discMat);
      grp.add(disc);

      // Chevrons mark the ring's "up" so vertical offset reads clearly.
      const chevronMat = new THREE.MeshBasicMaterial({
        color: 0x37e6ff, transparent: true, opacity: 0.9,
        blending: THREE.AdditiveBlending, depthWrite: false
      });
      for (let i = 0; i < 4; i++) {
        const a = (i / 4) * Math.PI * 2 + Math.PI / 4;
        const nub = new THREE.Mesh(new THREE.BoxGeometry(2.6, 2.6, 0.7), chevronMat);
        nub.position.set(Math.cos(a) * gate.radius, Math.sin(a) * gate.radius, 0);
        nub.rotation.z = a;
        grp.add(nub);
      }

      this.scene.add(grp);
      this.gateViews.push({ gate, grp, ringMat, discMat, chevronMat, ring, spin: Math.random() * 6 });
    }
  }

  // -------------------------------------------------------------------------
  _buildStructure() {
    const pylons = this.course.pylons;

    // --- bodies --------------------------------------------------------------
    const bodyGeo = new THREE.CylinderGeometry(1, 1.25, 1, 7, 1);
    bodyGeo.translate(0, 0.5, 0);
    const bodyMat = new THREE.MeshStandardMaterial({
      color: 0xffffff, roughness: 0.55, metalness: 0.35, flatShading: true
    });
    this.pylonMesh = new THREE.InstancedMesh(bodyGeo, bodyMat, pylons.length);
    this.pylonMesh.instanceMatrix.setUsage(THREE.StaticDrawUsage);

    // --- neon bands ----------------------------------------------------------
    const bandGeo = new THREE.CylinderGeometry(1.32, 1.32, 1, 7, 1, true);
    const bandMat = new THREE.MeshBasicMaterial({
      color: 0xffffff, transparent: true, opacity: 0.95,
      blending: THREE.AdditiveBlending, depthWrite: false
    });
    this.bandMesh = new THREE.InstancedMesh(bandGeo, bandMat, pylons.length);

    const cBody = new THREE.Color();
    const cBand = new THREE.Color();
    const KIND = {
      wall:   { body: 0x2a2540, band: 0x5a3f8f },
      slalom: { body: 0x3a2a12, band: 0xffb020 },
      spire:  { body: 0x33183a, band: 0xff3d81 }
    };

    for (let i = 0; i < pylons.length; i++) {
      const p = pylons[i];
      const h = Math.max(6, p.h);
      this._m.compose(
        this._v.set(p.x, p.y, p.z),
        this._q.identity(),
        this._s.set(p.r, h, p.r)
      );
      this.pylonMesh.setMatrixAt(i, this._m);
      cBody.setHex(KIND[p.kind].body);
      this.pylonMesh.setColorAt(i, cBody);

      const bandH = 2.4;
      this._m.compose(
        this._v.set(p.x, p.y + h - bandH, p.z),
        this._q.identity(),
        this._s.set(p.r, bandH, p.r)
      );
      this.bandMesh.setMatrixAt(i, this._m);
      cBand.setHex(KIND[p.kind].band);
      this.bandMesh.setColorAt(i, cBand);
    }
    this.pylonMesh.instanceMatrix.needsUpdate = true;
    this.bandMesh.instanceMatrix.needsUpdate = true;
    if (this.pylonMesh.instanceColor) this.pylonMesh.instanceColor.needsUpdate = true;
    if (this.bandMesh.instanceColor) this.bandMesh.instanceColor.needsUpdate = true;
    this.scene.add(this.pylonMesh, this.bandMesh);

    // --- floating shards -----------------------------------------------------
    const shards = this.course.shards;
    const shardGeo = new THREE.IcosahedronGeometry(1, 0);
    const shardMat = new THREE.MeshStandardMaterial({
      color: 0x1a2a4a, roughness: 0.35, metalness: 0.8, flatShading: true,
      emissive: 0x123a55, emissiveIntensity: 0.6
    });
    this.shardMesh = new THREE.InstancedMesh(shardGeo, shardMat, Math.max(1, shards.length));
    for (let i = 0; i < shards.length; i++) {
      const s = shards[i];
      this._m.compose(
        this._v.set(s.x, s.y, s.z),
        this._q.setFromEuler(new THREE.Euler(s.rot, s.rot * 0.7, s.rot * 0.3)),
        this._s.setScalar(s.s)
      );
      this.shardMesh.setMatrixAt(i, this._m);
    }
    this.shardMesh.instanceMatrix.needsUpdate = true;
    this.shardMesh.count = shards.length;
    this.scene.add(this.shardMesh);

    // --- boost pads ----------------------------------------------------------
    this.padViews = [];
    for (const pad of this.course.boostPads) {
      const grp = new THREE.Group();
      grp.position.copy(pad.center);
      this._q.setFromUnitVectors(new THREE.Vector3(0, 0, 1), pad.dir);
      grp.quaternion.copy(this._q);
      const mat = new THREE.MeshBasicMaterial({
        color: 0x7cffb2, transparent: true, opacity: 0.5,
        blending: THREE.AdditiveBlending, depthWrite: false, side: THREE.DoubleSide
      });
      const ring = new THREE.Mesh(new THREE.TorusGeometry(pad.radius, 0.7, 5, 28), mat);
      const inner = new THREE.Mesh(new THREE.RingGeometry(pad.radius * 0.25, pad.radius * 0.9, 26), mat);
      grp.add(ring, inner);
      this.scene.add(grp);
      this.padViews.push({ pad, grp, mat, inner });
    }
  }

  // -------------------------------------------------------------------------
  _buildStreaks() {
    const N = this.cfg.quality.streaks;
    this.streakN = N;
    this.streakLocal = new Float32Array(N * 3);
    for (let i = 0; i < N; i++) {
      this.streakLocal[i * 3] = (Math.random() * 2 - 1) * 75;
      this.streakLocal[i * 3 + 1] = (Math.random() * 2 - 1) * 55;
      this.streakLocal[i * 3 + 2] = Math.random() * 320 - 280;
    }
    const g = new THREE.BufferGeometry();
    this.streakPos = new Float32Array(N * 2 * 3);
    this.streakCol = new Float32Array(N * 2 * 3);
    g.setAttribute('position', new THREE.BufferAttribute(this.streakPos, 3));
    g.setAttribute('color', new THREE.BufferAttribute(this.streakCol, 3));
    g.boundingSphere = new THREE.Sphere(new THREE.Vector3(), 1e5);
    this.streaks = new THREE.LineSegments(g, new THREE.LineBasicMaterial({
      vertexColors: true, transparent: true, opacity: 0.7,
      blending: THREE.AdditiveBlending, depthWrite: false
    }));
    this.streaks.frustumCulled = false;
    this.scene.add(this.streaks);
  }

  // -------------------------------------------------------------------------
  update(dt, race, camera) {
    this._t += dt;
    const player = race.player;

    // Sky follows the player so the dome never runs out.
    this.sky.position.copy(player.position);
    this.stars.position.copy(player.position);

    // --- gate state ----------------------------------------------------------
    const nextIdx = race.nextGateIndex;
    for (let i = 0; i < this.gateViews.length; i++) {
      const gv = this.gateViews[i];
      const g = gv.gate;
      gv.spin += dt * (i === nextIdx ? 1.6 : 0.35);
      gv.ring.rotation.z = gv.spin;

      const dist = Math.abs(g.arc - player.arc);
      let color, ringOpacity, discOpacity, chevron;

      if (g.claimed) {
        color = 0x2ee66b; ringOpacity = 0.4; discOpacity = 0.05; chevron = 0.25;
      } else if (g.missed) {
        color = 0xff3b3b; ringOpacity = 0.55; discOpacity = 0.07; chevron = 0.4;
      } else if (i === nextIdx) {
        const pulse = 0.65 + 0.35 * Math.sin(this._t * 7);
        color = 0x9ff6ff; ringOpacity = 0.95 * pulse; discOpacity = 0.2 * pulse; chevron = 1;
      } else {
        color = 0x2f6f96;
        ringOpacity = clamp(0.55 - dist / 6000, 0.06, 0.55);
        discOpacity = 0.08; chevron = 0.45;
      }
      gv.ringMat.color.setHex(color);
      gv.ringMat.opacity = ringOpacity;
      gv.discMat.color.setHex(color);
      gv.discMat.opacity = discOpacity;
      gv.chevronMat.color.setHex(color);
      gv.chevronMat.opacity = chevron;

      // Only animate geometry we can actually see.
      gv.grp.visible = dist < 5200;
    }

    // --- boost pads ----------------------------------------------------------
    for (const pv of this.padViews) {
      pv.grp.visible = Math.abs(pv.pad.center.z - player.position.z) < 1600;
      if (!pv.grp.visible) continue;
      pv.inner.rotation.z += dt * 2.2;
      const pulse = 0.75 + 0.25 * Math.sin(this._t * 5 + pv.pad.center.z * 0.01);
      pv.mat.opacity = (pv.pad.used ? 0.12 : 0.45) * pulse;
    }

    // --- structure culling ---------------------------------------------------
    const pz = player.position.z, px = player.position.x;
    this.pylonMesh.visible = true;
    this.bandMesh.visible = true;
    this.shardMesh.visible = true;

    // --- speed streaks -------------------------------------------------------
    const spd = player.speed;
    const streakOn = clamp01((spd - 26) / 70);
    this.streaks.visible = streakOn > 0.02;
    if (this.streaks.visible) {
      const advance = spd * dt;
      const len = 4 + spd * 0.55;
      const mw = camera.matrixWorld.elements;
      const surge = player.surgeNorm;
      for (let i = 0; i < this.streakN; i++) {
        const i3 = i * 3;
        let lz = this.streakLocal[i3 + 2] + advance;
        if (lz > 40) {
          lz -= 320;
          this.streakLocal[i3] = (Math.random() * 2 - 1) * 75;
          this.streakLocal[i3 + 1] = (Math.random() * 2 - 1) * 55;
        }
        this.streakLocal[i3 + 2] = lz;
        const lx = this.streakLocal[i3], ly = this.streakLocal[i3 + 1];

        const ax = mw[0] * lx + mw[4] * ly + mw[8] * lz + mw[12];
        const ay = mw[1] * lx + mw[5] * ly + mw[9] * lz + mw[13];
        const az = mw[2] * lx + mw[6] * ly + mw[10] * lz + mw[14];
        const bx = mw[0] * 0 + mw[4] * 0 + mw[8] * (lz - len) + mw[12];
        const by = mw[1] * 0 + mw[5] * 0 + mw[9] * (lz - len) + mw[13];
        const bz = mw[2] * 0 + mw[6] * 0 + mw[10] * (lz - len) + mw[14];

        const o = i * 6;
        this.streakPos[o] = ax; this.streakPos[o + 1] = ay; this.streakPos[o + 2] = az;
        this.streakPos[o + 3] = bx; this.streakPos[o + 4] = by; this.streakPos[o + 5] = bz;

        const fade = clamp01(1 - Math.abs(lz) / 300) * streakOn;
        const r = 0.45 + surge * 0.55, gg = 0.85, b = 1.0;
        this.streakCol[o] = r * fade; this.streakCol[o + 1] = gg * fade; this.streakCol[o + 2] = b * fade;
        this.streakCol[o + 3] = 0; this.streakCol[o + 4] = 0; this.streakCol[o + 5] = 0;
      }
      this.streaks.geometry.attributes.position.needsUpdate = true;
      this.streaks.geometry.attributes.color.needsUpdate = true;
    }

    // --- fog tightens with speed for a tunnel-vision rush --------------------
    this.scene.fog.density = 0.00045 + player.speedNorm * 0.00025;
    void px; void pz;
  }
}
