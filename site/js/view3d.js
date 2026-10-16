// The optional 3D view: a PSP model with the live preview canvas on its
// screen. three.js loads on first use.

const $ = (id) => document.getElementById(id);
const THREE_URL = 'three';

// The model in site/models/ and the credit its license asks for, as
// { file, title, author, url, license, licenseUrl, changes }; null: no 3D view.
const MODEL = null;

export function offer(canvas, psp) {
  if (!MODEL) return;
  const model = MODEL;
  const credit = document.createElement('span');
  credit.append('3D model: ');
  const a = document.createElement('a');
  a.href = model.url;
  a.rel = 'noopener';
  a.textContent = `“${model.title}” by ${model.author}`;
  const lic = document.createElement('a');
  lic.href = model.licenseUrl;
  lic.rel = 'noopener';
  lic.textContent = model.license;
  credit.append(a, ', ', lic, model.changes ? ` (${model.changes})` : '', '.');
  $('model-credit').replaceChildren(credit.cloneNode(true));
  $('foot-credit').replaceChildren(credit);

  const button = $('open-3d');
  button.hidden = false;
  let view = null;
  button.addEventListener('click', async () => {
    $('view3d').showModal();
    if (!view) {
      button.disabled = true;
      try {
        view = await createView($('view3d-stage'), canvas, psp,
          new URL(`../models/${model.file}`, import.meta.url).href);
      } catch (err) {
        console.error(err);
        $('view3d-stage').textContent = 'The 3D view could not start in this browser.';
      }
      button.disabled = false;
    }
    view?.start();
  });
  $('close-3d').addEventListener('click', () => $('view3d').close());
  $('view3d').addEventListener('close', () => { view?.stop(); psp.releaseAll(); });
}

async function createView(stage, source, psp, url) {
  const THREE = await import(THREE_URL);
  const { GLTFLoader } = await import('three/addons/loaders/GLTFLoader.js');
  const { MeshoptDecoder } = await import('three/addons/libs/meshopt_decoder.module.js');

  const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
  renderer.outputColorSpace = THREE.SRGBColorSpace;
  renderer.toneMapping = THREE.ACESFilmicToneMapping;
  stage.append(renderer.domElement);

  const scene = new THREE.Scene();
  scene.add(new THREE.HemisphereLight(0xffffff, 0x222233, 1.6));
  const key = new THREE.DirectionalLight(0xffffff, 2.2);
  key.position.set(2, 3, 4);
  scene.add(key);
  const camera = new THREE.PerspectiveCamera(30, 1, 0.01, 10);
  camera.position.set(0, 0, 0.42);

  // The live preview on the screen mesh.
  const tex = new THREE.CanvasTexture(source);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.flipY = false; // glTF UV convention
  tex.magFilter = THREE.LinearFilter;
  tex.anisotropy = 4;
  const screenMat = new THREE.MeshBasicMaterial({ map: tex, toneMapped: false });

  // The screen maps to a strip of the model's texture atlas; stretch its UVs
  // so the whole 480x272 picture fills it.
  function fitUVs(geometry) {
    const uv = geometry.attributes.uv;
    let u0 = Infinity, u1 = -Infinity, v0 = Infinity, v1 = -Infinity;
    for (let i = 0; i < uv.count; i++) {
      u0 = Math.min(u0, uv.getX(i)); u1 = Math.max(u1, uv.getX(i));
      v0 = Math.min(v0, uv.getY(i)); v1 = Math.max(v1, uv.getY(i));
    }
    for (let i = 0; i < uv.count; i++)
      uv.setXY(i, (uv.getX(i) - u0) / (u1 - u0), (uv.getY(i) - v0) / (v1 - v0));
    uv.needsUpdate = true;
  }

  const gltf = await new GLTFLoader().setMeshoptDecoder(MeshoptDecoder).loadAsync(url);
  const model = gltf.scene;
  model.position.sub(new THREE.Box3().setFromObject(model).getCenter(new THREE.Vector3()));
  const pressable = {};
  model.traverse((o) => {
    const name = o.name.replace(/_/g, ' ');
    if (name === 'Screen' && o.isMesh) {
      o.material = screenMat;
      fitUVs(o.geometry);
    }
    if (['Arow Buttons', 'PS Buttons', 'L&R Bumpers'].includes(name)) pressable[name] = { obj: o, z: o.position.z };
  });
  const pivot = new THREE.Group();
  pivot.add(model);
  scene.add(pivot);

  // Which PSP button a hit on a button mesh is; centres in model space (metres).
  function buttonAt(hit) {
    let o = hit.object;
    while (o && !pressable[o.name.replace(/_/g, ' ')]) o = o.parent;
    if (!o) return null;
    const name = o.name.replace(/_/g, ' ');
    const p = model.worldToLocal(hit.point.clone());
    const quad = (cx, cy, up, down, left, right) => {
      const dx = p.x - cx, dy = p.y - cy;
      return Math.abs(dx) > Math.abs(dy) ? (dx < 0 ? left : right) : (dy > 0 ? up : down);
    };
    if (name === 'Arow Buttons') return { name, btn: quad(-0.0641, 0.0031, 'up', 'down', 'left', 'right') };
    if (name === 'PS Buttons') return { name, btn: quad(0.0651, 0.0033, 'triangle', 'cross', 'square', 'circle') };
    return { name, btn: p.x < 0 ? 'l' : 'r' };
  }

  // Drag to turn; a press on a button presses it.
  const el = renderer.domElement;
  const raycaster = new THREE.Raycaster();
  const ndc = new THREE.Vector2();
  let drag = null, pressed = null;
  const rot = { x: 0.15, y: -0.25 };
  el.addEventListener('pointerdown', (e) => {
    el.setPointerCapture(e.pointerId);
    const r = el.getBoundingClientRect();
    ndc.set(((e.clientX - r.left) / r.width) * 2 - 1, -((e.clientY - r.top) / r.height) * 2 + 1);
    raycaster.setFromCamera(ndc, camera);
    const hit = raycaster.intersectObject(model, true)[0];
    const b = hit && buttonAt(hit);
    if (b) {
      pressed = { ...b, node: pressable[b.name] };
      pressed.node.obj.position.z = pressed.node.z - 0.0009;
      psp.press('3d', b.btn);
    } else {
      drag = { x: e.clientX, y: e.clientY, rx: rot.x, ry: rot.y };
    }
  });
  const up = () => {
    if (pressed) {
      pressed.node.obj.position.z = pressed.node.z;
      psp.release('3d', pressed.btn);
    }
    pressed = drag = null;
  };
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
  el.addEventListener('pointermove', (e) => {
    if (!drag) return;
    rot.y = drag.ry + (e.clientX - drag.x) * 0.006;
    rot.x = Math.max(-1.2, Math.min(1.2, drag.rx + (e.clientY - drag.y) * 0.006));
  });

  function resize() {
    const { clientWidth: w, clientHeight: h } = stage;
    renderer.setSize(w, h, false);
    camera.aspect = w / h;
    // Fit the PSP's width (about 0.17 m) with some margin.
    const fit = 0.2 / (2 * Math.tan(THREE.MathUtils.degToRad(camera.fov / 2)) * camera.aspect);
    camera.position.z = Math.max(0.3, fit);
    camera.updateProjectionMatrix();
  }
  new ResizeObserver(resize).observe(stage);

  return {
    start() {
      resize();
      renderer.setAnimationLoop(() => {
        pivot.rotation.set(rot.x, rot.y, 0);
        tex.needsUpdate = true;
        renderer.render(scene, camera);
      });
    },
    stop() { renderer.setAnimationLoop(null); },
  };
}
