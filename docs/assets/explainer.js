// Interactive figures for the local-siri explainer. No dependencies; each figure is a
// <figure class="widget" data-widget="..."> filled in here. Numbers are the ones measured on the iPod.
(() => {
  const $ = (root, sel) => root.querySelector(sel);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  // Text with \( inline LaTeX \) for generated readouts: the maths is typeset by KaTeX (loaded before this script), the rest is escaped
  const esc = (t) => t.replace(/[&<>]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;' }[c]));
  const tex = (m) => (window.katex ? window.katex.renderToString(m, { throwOnError: false }) : esc(m));
  const mathText = (str) => str.split(/(\\\(.*?\\\))/).map((part) => (part.startsWith('\\(') && part.endsWith('\\)') ? tex(part.slice(2, -2)) : esc(part))).join('');
  const setMath = (e, str) => { e.innerHTML = mathText(str); return e; };
  const mel = (tag, cls, str) => setMath(el(tag, cls), str);
  // Matrices shared by the grouping figure and the 2D figure: weights with each group's scale printed on them, a heatmap of the scales, and a heatmap of the rounding errors
  const SVGNS = 'http://www.w3.org/2000/svg';
  const sv = (tag, attrs, style) => { const e = document.createElementNS(SVGNS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); if (style) e.style.cssText = style; return e; };
  const divColor = (v, clip) => `color-mix(in srgb, var(${v >= 0 ? '--sx-red' : '--sx-blue'}) ${Math.min(100, Math.abs(v) / clip * 100)}%, var(--bg))`;   // blue negative, red positive
  const redColor = (e, clip) => `color-mix(in srgb, var(--sx-red) ${Math.min(100, e / clip * 100)}%, var(--bg))`;                                        // the redder, the larger
  const goldColor = (v, max) => `color-mix(in srgb, var(--sx-gold) ${Math.max(10, v / max * 100)}%, var(--bg))`;                                        // the more gold, the larger
  // White lines: between the groups of each row, and (rows) between the rows. R rows of C columns, groups of gs columns, cells CW by RH, shifted right by x0.
  const gridLines = ({ R, C, CW, RH, gs, x0 = 0, rows }) => {
    const kids = [], ng = C / gs;
    if (rows) for (let r = 1; r < R; r++) kids.push(sv('line', { x1: x0, x2: x0 + C * CW, y1: r * RH, y2: r * RH, class: 'gl' }));
    for (let r = 0; r < R; r++) for (let g = 1; g < ng; g++) kids.push(sv('line', { x1: x0 + g * gs * CW, x2: x0 + g * gs * CW, y1: r * RH, y2: (r + 1) * RH, class: 'gl' }));
    return kids;
  };
  // One rectangle per weight. color(r, c) fills it; label(r, g), if given, prints a number on each group; outline(r, c), if given, marks a cell.
  const matrixCells = ({ R, C, CW, RH, gs, x0 = 0, rows, color, label, outline }) => {
    const kids = [], ng = C / gs;
    for (let r = 0; r < R; r++) for (let c = 0; c < C; c++) kids.push(sv('rect', { x: x0 + c * CW, y: r * RH, width: CW + .4, height: RH + .4 }, `fill:${color(r, c)}`));
    kids.push(...gridLines({ R, C, CW, RH, gs, x0, rows: rows === undefined ? ng > 1 : rows }));
    if (outline) for (let r = 0; r < R; r++) for (let c = 0; c < C; c++) if (outline(r, c)) kids.push(sv('rect', { x: x0 + c * CW + .75, y: r * RH + .75, width: CW - 1.5, height: RH - 1.5, class: 'olc' }));
    if (label) for (let r = 0; r < R; r++) for (let g = 0; g < ng; g++) { const t = sv('text', { x: x0 + (g + .5) * gs * CW, y: r * RH + RH * .5, 'text-anchor': 'middle', 'dominant-baseline': 'central', class: 'gv' }); t.textContent = label(r, g); kids.push(t); }
    return kids;
  };
  // One block per group, as wide as the group: for the heatmap of the scales. color(r, g) fills it.
  const matrixGroups = ({ R, C, CW, RH, gs, x0 = 0, rows, color, outline }) => {
    const kids = [], ng = C / gs;
    for (let r = 0; r < R; r++) for (let g = 0; g < ng; g++) kids.push(sv('rect', { x: x0 + g * gs * CW, y: r * RH, width: gs * CW + .4, height: RH + .4 }, `fill:${color(r, g)}`));
    kids.push(...gridLines({ R, C, CW, RH, gs, x0, rows: rows === undefined ? ng > 1 : rows }));
    if (outline) for (let r = 0; r < R; r++) for (let g = 0; g < ng; g++) if (outline(r, g)) kids.push(sv('rect', { x: x0 + g * gs * CW + 1, y: r * RH + 1, width: gs * CW - 2, height: RH - 2, class: 'olg' }));   // marks a group, e.g. one that contains an outlier
    return kids;
  };

  // The grouping figure publishes its current groups here (the average rounding error of each, and whether it holds an outlier), so a later figure can show the same data
  const groupingShare = { data: null, subs: [] };
  const publishGrouping = (d) => { groupingShare.data = d; groupingShare.subs.forEach((f) => f(d)); };
  const press = (buttons, on) => buttons.forEach((b) => b.setAttribute('aria-pressed', String(b === on)));

  // Jacobi rotations for a small symmetric matrix: eigenvectors (biggest eigenvalue first) and eigenvalues
  const jacobi = (C) => {
    const D = C.length, A = C.map((r) => r.slice()), V = Array.from({ length: D }, (_, i) => Array.from({ length: D }, (_, j) => (i === j ? 1 : 0)));
    for (let sweep = 0; sweep < 60; sweep++) for (let p = 0; p < D; p++) for (let q = p + 1; q < D; q++) {
      if (Math.abs(A[p][q]) < 1e-14) continue;
      const th = (A[q][q] - A[p][p]) / (2 * A[p][q]), t = Math.sign(th || 1) / (Math.abs(th) + Math.sqrt(th * th + 1)), c = 1 / Math.sqrt(t * t + 1), sn = t * c;
      for (let k = 0; k < D; k++) { const a = A[k][p], b = A[k][q]; A[k][p] = c * a - sn * b; A[k][q] = sn * a + c * b; }
      for (let k = 0; k < D; k++) { const a = A[p][k], b = A[q][k]; A[p][k] = c * a - sn * b; A[q][k] = sn * a + c * b; }
      for (let k = 0; k < D; k++) { const a = V[k][p], b = V[k][q]; V[k][p] = c * a - sn * b; V[k][q] = sn * a + c * b; }
    }
    const order = A.map((r, i) => i).sort((a, b) => A[b][b] - A[a][a]);
    return { vecs: order.map((i) => V.map((row) => row[i])), vals: order.map((i) => A[i][i]) };
  };

  // A "spin the view" switch for the 3D figures: turns the camera about the vertical axis while it is on and on screen
  const spin = (fig, state, draw, busy) => {
    const b = $(fig, '[data-act=spin]'); let raf = 0, last = 0, seen = true;
    const on = () => b.getAttribute('aria-pressed') === 'true';
    const tick = (t) => {
      if (!on() || !seen || document.hidden) { raf = 0; return; }
      const dt = last ? Math.min(.05, (t - last) / 1000) : 0; last = t;
      if (!busy()) { state.yaw += dt * .7; draw(); }
      raf = requestAnimationFrame(tick);
    };
    const start = () => { if (!raf) { last = 0; raf = requestAnimationFrame(tick); } };
    if ('IntersectionObserver' in window) new IntersectionObserver((es) => { seen = es[0].isIntersecting; if (seen && on()) start(); }).observe(fig);
    document.addEventListener('visibilitychange', () => { if (!document.hidden && on()) start(); });
    b.addEventListener('click', () => { const v = !on(); b.setAttribute('aria-pressed', String(v)); if (v) start(); });
  };

  // 1. Where each piece of Siri comes from
  const inventory = (fig) => {
    const items = [
      ['ipod', 'AssistantServices.framework', 'binary in the iPod\'s cache', 'Already in the iPod\'s dyld shared cache: the client side of Siri that SpringBoard and Settings use to talk to assistantd. Only its resource files (Info.plist, strings) are missing.'],
      ['ipod', 'SAObjects.framework', 'binary in the iPod\'s cache', 'The Objective-C classes for every ACE object (SASpeechRecognized, SAUIAddViews, ...). Also already in the iPod\'s cache.'],
      ['ipod', 'VoiceServices / voiced', 'speech synthesis', 'Siri speaks with the same text-to-speech engine VoiceOver uses, which every iPod has.'],
      ['ipod', 'AVVoiceController', 'recording', 'Records your voice and decides when you have stopped talking. Present, but tuned for the 4S: see section 5.'],
      ['ipod', 'MobileGestalt', 'says "no Siri here"', 'The capability database. It answers false for the "assistant" key, so the tweak hooks it (section 4).'],
      ['file', 'assistantd', 'the Siri daemon', 'A normal executable in AssistantServices.framework on the 4S. It records audio, keeps the ACE session with the server, and relays results to SpringBoard. All 35 libraries it links are on the iPod.'],
      ['file', 'Assistant.uibundle', 'SpringBoard plugin', 'The Siri screen itself: the paper-texture bubbles, the glowing microphone. A loadable bundle, not in the cache.'],
      ['file', '/System/Library/Assistant', 'UI plugins', 'Snippet plugins for maps, weather, stocks, reminders and so on. They load, but this project never sends their objects.'],
      ['file', 'Settings, LaunchDaemons', 'and resources', 'Siri\'s page in Settings, the launchd jobs for assistantd and assistant_service, and the frameworks\' resource folders: about 45 MB in all.'],
      ['cache', 'AssistantUI.framework', '137 KB, repaired', 'The only binary that exists solely inside the 4S\'s shared cache. Extracting it and making it load elsewhere is section 3.'],
      ['ours', 'SiriCapability tweak', 'Substrate', 'Says yes to the capability check, makes recording work without a Speex encoder, replaces the end-of-speech detection, and disables timeouts sized for Apple\'s servers.'],
      ['ours', 'acesrv', 'ACE server', 'Stands in for guzzoni.apple.com on 127.0.0.1:4443 (section 6).'],
      ['ours', 'localsirid', 'speech + LLM', 'Whistle and SmolLM2-135M in C and NEON (section 8).'],
      ['ours', '/etc/hosts entry', 'guzzoni.apple.com → 127.0.0.1', 'Makes sure Siri can only ever reach the iPod itself (section 7).'],
    ];
    const grid = $(fig, '.sx-inv'), note = $(fig, '.sx-note');
    const buttons = items.map(([src, name, sub, text]) => {
      const b = el('button', 'sx-item'); b.dataset.src = src; b.type = 'button';
      b.append(name, el('small', null, sub));
      b.addEventListener('click', () => { press(buttons, b); note.textContent = text; });
      grid.append(b); return b;
    });
  };

  // 2. Repairing the decached library
  const repair = (fig) => {
    const kinds = { rebase: 40, bind: 10, lazy: 6, stub: 2, objc: 6 };
    const notes = {
      load: 'In SpringBoard, dyld placed AssistantUI 0x2D46B000 bytes (about 760 MB) lower than in the 4S cache. Every hatched slot now holds an address from the 4S memory map: following it would crash, as SpringBoard did.',
      rebase: 'Pointers into AssistantUI itself (class lists, string data, method tables). The cache slides as one block, so the library\'s own rebase list was deleted. Rebuilt from the cache\'s slide table: every slot it marks that points back into the library gets REBASE_TYPE_POINTER. 2,479 of them.',
      bind: 'Pointers into other libraries that the cache builder resolved once and forgot: the isa of each constant string, every class\'s empty method cache and vtable, block isa, non-lazy symbol pointers. Names are recovered from the indirect symbol table or the 4S cache\'s exports, and bind opcodes are written for them. 204 of them.',
      lazy: 'Lazy pointers (objc_msgSend, CGRectInset, ...) were pre-resolved to 4S addresses, so the first call would jump into nowhere. Their lazy-bind opcodes survived; copying them into the regular bind stream makes dyld fill them at load time. 61 of them.',
      stub: 'The cache builder aimed three call stubs (dispatch_once, dispatch_async, dispatch_get_global_queue) at libdispatch\'s own pointers in the 4S cache. Calling dispatch_once jumped to 0xfff0f0f0 and SpringBoard crashed. Re-pointed at AssistantUI\'s own lazy pointers.',
      objc: 'Method lists were flagged "selectors already uniqued by the cache", so libobjc would trust names that now point at private copies. Clearing the flag makes the runtime register them again, and messages find their methods. 47 of 54 lists.',
      reset: 'In the cache, every slot was correct for the 4S\'s memory layout.',
    };
    const mem = $(fig, '.sx-mem'), note = $(fig, '.sx-note');
    const cells = [];
    Object.entries(kinds).forEach(([k, n]) => { for (let i = 0; i < n; i++) cells.push(k); });
    for (let i = cells.length; i < 80; i++) cells.push('');
    // deterministic shuffle so the picture looks like real memory
    let seed = 7; const rnd = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
    for (let i = cells.length - 1; i > 0; i--) { const j = Math.floor(rnd() * (i + 1)); [cells[i], cells[j]] = [cells[j], cells[i]]; }
    const divs = cells.map((k) => { const d = el('div', k ? 'k-' + k : ''); d.dataset.k = k; d.title = k || 'plain data'; mem.append(d); return d; });
    const fixed = new Set(); let loaded = false;
    const draw = () => divs.forEach((d) => d.classList.toggle('broken', loaded && d.dataset.k && !fixed.has(d.dataset.k)));
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      const act = b.dataset.act, fix = b.dataset.fix;
      if (act === 'load') loaded = true;
      if (act === 'reset') { loaded = false; fixed.clear(); fig.querySelectorAll('[data-fix]').forEach((x) => x.setAttribute('aria-pressed', 'false')); }
      if (fix) { loaded = true; fixed.add(fix); b.setAttribute('aria-pressed', 'true'); }
      note.textContent = notes[act || fix];
      if (loaded && fixed.size === 5) note.textContent += ' All repaired: AssistantUI loads on the iPod and Siri\'s screen appears.';
      draw();
    }));
    note.textContent = notes.reset;
  };

  // 3. An ACE conversation (recorded from the iPod's assistantd)
  const ace = (fig) => {
    const steps = [
      ['c', 'ACE /ace HTTP/1.0 · AA CC EE 02', 'Siri opens the connection with an HTTP-looking request line, then the stream magic. From here on, each direction is one zlib stream.'],
      ['s', 'HTTP/1.1 200 OK · AA CC EE 02', 'acesrv answers in kind.'],
      ['c', 'LoadAssistant', 'Siri asks for its account by id. The first time, it sends CreateAssistant instead, and saves the ids acesrv hands back.'],
      ['s', 'AssistantLoaded', 'No authentication: assistantd skips it because the preferences say "Authentication Disabled".'],
      ['c', 'SetRestrictions · ClearContext', 'Context the real server would use. acesrv needs no reply for these.'],
      ['c', 'GetAnchors', 'Siri wants to sync contacts, apps and so on, and won\'t send any speech until this is answered.'],
      ['s', 'GetAnchorsResponse (no anchors)', 'acesrv keeps no synced data, so it reports none.'],
      ['c', 'StartSpeechRequest  Speex_WB_Quality8', 'Siri heard you start talking. acesrv tells localsirid to read the speech model into RAM now, while you are still speaking.'],
      ['c', 'SpeechPacket × 12', 'Your voice: PCM or Speex, whatever the label says. acesrv tells them apart by data rate.'],
      ['c', 'Chunk', 'A sync chunk arrives in the middle of the speech.'],
      ['s', 'ChunkAccepted', 'acesrv accepts every chunk, echoing its new generation.'],
      ['c', 'FinishSpeech', 'The tweak\'s end-of-speech detection decided you stopped. Audio goes to localsirid on a worker thread; the main loop keeps answering pings.'],
      ['s', 'SpeechRecognized  "What is the capital of France?"', 'Sent as soon as speech recognition finishes, about 3 s later, so your words appear while the language model works.'],
      ['s', 'AddViews  AssistantUtteranceView  "The capital of France is Paris."', 'The answer, with speakableText, so Siri also reads it aloud.'],
      ['s', 'RequestCompleted', 'Done. Siri plays its end tone and speaks.'],
      ['c', 'Metrics', 'Siri reports timing data to the server, which acesrv ignores.'],
    ];
    const box = $(fig, '.sx-ace'), note = $(fig, '.sx-note');
    box.append(el('div', 'sx-hdr left', 'Siri (assistantd)'), el('div', 'sx-hdr', ''), el('div', 'sx-hdr right', 'acesrv'));
    const rows = steps.map(([who, text]) => {
      const m = el('div', 'msg ' + (who === 'c' ? 'left' : 'right'), text);
      const a = el('div', 'arrow', who === 'c' ? '→' : '←');
      if (who === 'c') box.append(m, a, el('div')); else box.append(el('div'), a, m);
      return [m, a];
    });
    let n = 0;
    const show = (k) => {
      n = k;
      rows.forEach(([m, a], i) => { m.classList.toggle('shown', i < n); a.classList.toggle('shown', i < n); m.classList.toggle('current', i === n - 1); });
      note.textContent = n ? steps[n - 1][2] : 'Pings and pongs flow in both directions throughout; they are left out here.';
    };
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.act === 'next') show(n >= steps.length ? 0 : n + 1);
      if (b.dataset.act === 'all') show(steps.length);
      if (b.dataset.act === 'reset') show(0);
    }));
    show(0);
  };

  // 4. RAM budget during a question
  const ram = (fig) => {
    const state = { siri: true, prefetch: true, cq: true };
    const bar = $(fig, '.sx-bar'), note = $(fig, '.sx-note');
    const draw = () => {
      const parts = [
        ['reserved: GPU, firmware', 60, '#4a4a4a'],
        ['wired: kernel, drivers', 46, '#6b6b6b'],
        ['daemons', 34, '#8a8a8a'],
        ['SpringBoard', 20, '#2f6db3'],
        ...(state.siri ? [['Siri: UI, assistantd, voice', 25, '#7a4fd0']] : []),
        ['other file cache', 20, '#a0a0a0'],
        state.cq ? ['Whistle', 17, '#c88d0c'] : ['Moonshine', 30, '#c88d0c'],
        ['SmolLM2 working set', state.cq ? 76 : 80, '#2e7d4f'],
      ];
      const total = parts.reduce((s, p) => s + p[1], 0), over = Math.max(0, total - 256);
      bar.replaceChildren(...parts.map(([name, mb, c]) => {
        const d = el('div', null, `${name} ${mb}`); d.style.flex = `0 0 ${(mb / Math.max(256, total)) * 100}%`; d.style.background = c; d.title = `${name}: ~${mb} MB`; return d;
      }));
      if (over) { const d = el('div', 'over', ''); d.style.flex = `0 0 ${(over / total) * 100}%`; d.title = `${over} MB more than the iPod has`; bar.append(d); }
      const lm = state.cq ? 76 : 80, speed = state.prefetch ? 26 : 6, reload = lm / speed;   // measured with Siri open
      note.textContent = over
        ? `Needs ~${total} MB of 256: the models can't both stay resident, so each stage evicts the other. Bringing SmolLM2's ${lm} MB back takes ~${reload.toFixed(reload < 2 ? 1 : 0)} s at ${speed} MB/s${state.prefetch ? ' (one sequential read)' : ' (one page fault at a time)'}.`
        : `Needs ~${total} MB of 256: both models stay in RAM between questions, and nothing has to be read back from flash.`;
    };
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      state[b.dataset.k] = !state[b.dataset.k]; b.setAttribute('aria-pressed', String(state[b.dataset.k])); draw();
    }));
    draw();
  };

  // 5. Timeline from the end of speech to the full answer
  const timeline = (fig) => {
    const data = {
      before: { label: 'First version inside Siri: every stage pages its model in from flash one fault at a time.', rows: [['speech recognition', 'asr', 6.4], ['prompt (13 tokens)', 'prompt', 15.2], ['answer (7 tokens)', 'gen', 5.7]] },
      after: { label: 'Speech model prefetched while you talk, silence trimmed, the LLM read sequentially once the transcript is out.', rows: [['speech recognition', 'asr', 2.4], ['LLM prefetch', 'load', 2.9], ['prompt (13 tokens)', 'prompt', 6.4], ['answer (7 tokens)', 'gen', 2.5]] },
      now: { label: 'Whistle and the 4-bit codebook SmolLM2, with the model locked in RAM from its read-back to the end of the answer, so no page faults. The answer is also more often right.', rows: [['speech recognition', 'asr', 2.7], ['LLM prefetch', 'load', 2.8], ['prompt (13 tokens)', 'prompt', 3.2], ['answer (7 tokens)', 'gen', 2.8]] },
      alone: { label: 'For reference: the same question straight to localsirid, Siri closed, models warm.', rows: [['speech recognition', 'asr', 1.35], ['prompt (13 tokens)', 'prompt', 1.8], ['answer (7 tokens)', 'gen', 1.7]] },
    };
    const MAX = 28, box = $(fig, '.sx-tl'), note = $(fig, '.sx-note'), buttons = [...fig.querySelectorAll('button')];
    const draw = (v) => {
      const d = data[v]; let t = 0;
      const rows = d.rows.map(([name, cls, s]) => {
        const row = el('div', 'row'), track = el('div', 'track');
        if (t) { const w = el('div', 'seg wait'); w.style.flex = `0 0 ${(t / MAX) * 100}%`; w.style.background = 'transparent'; track.append(w); }
        const seg = el('div', 'seg ' + cls, `${s} s`); seg.style.flex = `0 0 ${(s / MAX) * 100}%`; track.append(seg);
        row.append(el('div', 'lbl', name), track); t += s; return row;
      });
      const axis = el('div', 'axis'); const ticks = el('div');
      [0, 7, 14, 21, 28].forEach((x) => ticks.append(el('span', null, `${x} s`)));
      axis.append(el('div'), ticks);
      box.replaceChildren(...rows, axis);
      note.textContent = `${d.label} Total: ${t.toFixed(1)} s.`;
    };
    buttons.forEach((b) => b.addEventListener('click', () => { press(buttons, b); draw(b.dataset.v); }));
    draw('before');
  };

  // Walsh–Hadamard helpers (unnormalized ±1 entries; divide by √n where it matters)
  const hadamard = (n) => {
    let h = [[1]];
    while (h.length < n) h = [...h.map((r) => [...r, ...r]), ...h.map((r) => [...r, ...r.map((v) => -v)])];
    return h;
  };
  const rotate = (v) => { const n = v.length, h = hadamard(n), s = Math.sqrt(n); return h.map((row) => row.reduce((a, x, j) => a + x * v[j], 0) / s); };

  // 6. Building a Walsh–Hadamard matrix by doubling
  const hadamardFig = (fig) => {
    const box = $(fig, '.sx-had'), note = $(fig, '.sx-note'), buttons = [...fig.querySelectorAll('button')];
    const draw = (n) => {
      const h = hadamard(n), grid = el('div', 'grid');
      grid.style.gridTemplateColumns = `repeat(${n}, 1fr)`;
      grid.style.maxWidth = `${Math.min(26, n * 2.4)}rem`;
      h.forEach((row, i) => row.forEach((v, j) => {
        const c = el('div', v > 0 ? 'p' : 'm', n <= 8 ? (v > 0 ? '+' : '−') : '');
        if (n > 2 && i >= n / 2 && j >= n / 2) c.classList.add('neg');   // the negated copy
        c.dataset.row = i;
        c.addEventListener('click', () => {
          grid.classList.add('picked');
          grid.querySelectorAll('div').forEach((d) => d.classList.toggle('sel', d.dataset.row === String(i)));
          const terms = row.map((x, k) => `${x > 0 ? (k ? ' + ' : '') : (k ? ' - ' : '-')}x_{${k + 1}}`).join('');
          const flips = row.slice(1).reduce((a, x, k) => a + (x !== row[k] ? 1 : 0), 0);
          setMath(note, `Output ${i + 1}: \\(y_{${i + 1}} = \\frac{1}{\\sqrt{${n}}}\\left(${terms}\\right)\\). Every input counts equally, only the signs differ. The sign flips ${flips} times along this row: rows are like square waves of different frequencies, and any two rows agree in exactly half their places, which is what makes them orthogonal.`);
        });
        grid.append(c);
      }));
      box.replaceChildren(grid);
      setMath(note, n === 2
        ? 'The 2×2 matrix: output 1 is the sum of the two inputs, output 2 their difference (each divided by \\(\\sqrt{2}\\)).'
        : `${n}×${n}: four copies of the ${n / 2}×${n / 2} matrix, the bottom-right one (outlined) with its signs flipped. Click a row.`);
    };
    buttons.forEach((b) => b.addEventListener('click', () => { press(buttons, b); draw(+b.dataset.n); }));
    draw(16);
  };

  // 7. Quantizing one group: evenly spaced levels vs rotation + Gaussian codebook
  const cqFig = (fig) => {
    const G = 32, SIGMA = 0.02;   // group size shown; weight scale
    const CB = {   // Lloyd–Max levels for a unit Gaussian, as in Whistle and convert/cq.py
      2: [-1.507, -0.451, 0.453, 1.51],
      4: [-2.71, -2.044, -1.595, -1.233, -0.922, -0.64, -0.373, -0.114, 0.144, 0.404, 0.673, 0.961, 1.276, 1.637, 2.084, 2.741],
    };
    const state = { rotate: true, codebook: true, heavy: true, bits4: false };
    const rng = (seed) => () => (seed = (seed * 16807) % 2147483647) / 2147483647;
    const gauss = (r) => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r());
    // heavy tails: Student's t with 3 degrees of freedom, a common model for weights with outliers
    const sample = (r) => state.heavy ? gauss(r) / Math.sqrt((gauss(r) ** 2 + gauss(r) ** 2 + gauss(r) ** 2) / 3) : gauss(r);
    const group = (r) => Array.from({ length: G }, () => sample(r) * SIGMA);
    const norm = (v) => Math.sqrt(v.reduce((a, x) => a + x * x, 0));
    const nearest = (levels, x) => levels.reduce((b, l) => (Math.abs(l - x) < Math.abs(b - x) ? l : b), levels[0]);
    // Quantize one group with the current settings. Returns display values (in level units), the levels,
    // and the error after decoding, relative to the group's own size (rms).
    const quant = (w) => {
      const bits = state.bits4 ? 4 : 2, n = 1 << bits, v = state.rotate ? rotate(w) : w;
      let s, levels;
      if (state.codebook) { s = norm(v) / Math.sqrt(G); levels = CB[bits]; }          // length-scaled Gaussian codebook
      else {                                                                          // evenly spaced up to the largest value
        const m = Math.max(...v.map(Math.abs)); s = m / 3;
        levels = Array.from({ length: n }, (_, k) => (-m + (2 * m / n) * (k + 0.5)) / s);
      }
      const shown = v.map((x) => x / s), q = shown.map((x) => nearest(levels, x) * s);
      const back = state.rotate ? rotate(q) : q;                                      // the rotation is its own inverse
      const err = norm(w.map((x, i) => x - back[i])) / norm(w);
      return { shown, levels, err, n };
    };
    let seed = 11;
    const svg = $(fig, 'svg'), note = $(fig, '.sx-note'), NS = 'http://www.w3.org/2000/svg';
    const mk = (tag, attrs) => { const e = document.createElementNS(NS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); return e; };
    const draw = () => {
      const w = group(rng(seed)), { shown, levels, err, n } = quant(w);
      const r = rng(4242); let avg = 0; for (let i = 0; i < 500; i++) avg += quant(group(r)).err / 500;
      const big = w.reduce((b, x, i) => (Math.abs(x) > Math.abs(w[b]) ? i : b), 0);
      const R = Math.max(1.6, ...shown.map(Math.abs), ...levels.map(Math.abs)) * 1.08, X = (x) => 20 + ((x + R) / (2 * R)) * 600;
      const kids = [mk('line', { x1: 20, x2: 620, y1: 112, y2: 112, class: 'axis' })];
      levels.forEach((l) => kids.push(mk('line', { x1: X(l), x2: X(l), y1: 104, y2: 120, class: 'level' })));
      shown.forEach((x, i) => {
        const y = 18 + (i % 6) * 13;
        kids.push(mk('line', { x1: X(x), x2: X(nearest(levels, x)), y1: y, y2: 104, class: 'snap' }));
        kids.push(mk('circle', { cx: X(x), cy: y, r: 4, class: !state.rotate && i === big ? 'dot out' : 'dot' }));
      });
      const t = mk('text', { x: 20, y: 142, class: 'lbl' }); t.textContent = `${n} levels`; kids.push(t);
      svg.replaceChildren(...kids);
      const used = new Set(shown.map((x) => nearest(levels, x))).size;
      note.textContent = `${state.rotate ? 'Rotated' : 'Raw'} weights, ${state.codebook ? 'Gaussian codebook' : 'evenly spaced levels'}, ${state.bits4 ? 4 : 2} bits: these ${G} values use ${used} of ${n} levels. `
        + `Error after decoding: ${(err * 100).toFixed(0)}% of the group's size here, ${(avg * 100).toFixed(0)}% on average over 500 random groups.`
        + (!state.rotate && !state.codebook ? ' The largest weight (red) sets the spacing, so the ordinary weights share very few levels.' : '')
        + (state.rotate && state.codebook ? ' This is the combination the iPod runs.' : '');
    };
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.act === 'new') seed = (seed * 48271) % 2147483647;
      else { state[b.dataset.k] = !state[b.dataset.k]; b.setAttribute('aria-pressed', String(state[b.dataset.k])); }
      draw();
    }));
    draw();
  };

  // 6-. Groups of two weights, in stages. Figures with the same data-scene share their mock groups (and the levels option); data-layers says what each one draws:
  // strips (three lines of cells), points, quant (squares, grids, arrows and rings), bits (binary labels), ghost (where the groups started), curve (spikiness against angle), loss (how much each group loses).
  // A group has a direction and a length; its two weights are length times (cos, sin). With "one fixed grid" the length is divided out, so the dots sit on a circle of length 1.
  const rng2 = (sd) => () => (sd = (sd * 16807) % 2147483647) / 2147483647;
  const turn2 = (p, d) => { const c = Math.cos(d * Math.PI / 180), s = Math.sin(d * Math.PI / 180); return [p[0] * c - p[1] * s, p[0] * s + p[1] * c]; };
  const scenes = {};
  // The groups run from one extreme to the other: the first is short and lies on an axis, the last is long and lies on a diagonal, and the groups in between are spread evenly
  // in length (0.35 to 1.35) and in direction (0 to 45 degrees). The first half are closer to an axis ("spiky", triangles), the second half closer to a diagonal ("flat", circles).
  // With data-groups="clusters" (the Hadamard-angle figure) the two halves are instead two clusters, each spread evenly on both sides of its direction (an axis, and a diagonal), so that the troughs of the curves sit exactly at 45 degrees.
  const groupsOf = (sc) => {
    const n = sc.st.n, half = n / 2;
    if (sc.groups === 'clusters') { const off = (j) => (half > 1 ? (j / (half - 1) - .5) * 8 : 0); return Array.from({ length: n }, (_, i) => { const spiky = i < half, a = ((spiky ? 0 : 45) + off(spiky ? i : i - half)) * Math.PI / 180; return { dir: [Math.cos(a), Math.sin(a)], len: .35 + (n > 1 ? i / (n - 1) : 0), spiky }; }); }
    return Array.from({ length: n }, (_, i) => { const t = n > 1 ? i / (n - 1) : 0, a = 45 * t * Math.PI / 180; return { dir: [Math.cos(a), Math.sin(a)], len: .35 + t, spiky: i < half }; });
  };
  const rot2dFig = (fig) => {
    const had = fig.hasAttribute('data-had'), layers = (fig.dataset.layers || 'points').split(/\s+/), has = (l) => layers.includes(l);
    const sc = scenes[fig.dataset.scene || 'main'] || (scenes[fig.dataset.scene || 'main'] = { st: { n: 4, mode: 'plain' }, subs: [], groups: fig.dataset.groups || 'ramp' });
    if (fig.dataset.levels) sc.st.mode = fig.dataset.levels;      // a figure can fix the levels option: "fixed" is step 3's one shared grid
    const rot = { deg: 0, sel: 0, follow: false };      // per figure: only the rotation figures turn the groups
    const vis = {}, v = (k) => vis[k] !== false;         // which elements are shown: switched from the row of toggles above the plot
    const anim = { a: 1, raf: 0 }, ease = (t) => (t < .5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2);     // how far a trail figure's groups have moved, 0 to 1
    const play = () => {                                // slide the groups from their real weights to where they are stored
      cancelAnimationFrame(anim.raf);
      if (matchMedia('(prefers-reduced-motion: reduce)').matches) { anim.a = 1; draw(); return; }
      const t0 = performance.now(), step = (t) => { anim.a = Math.min(1, (t - t0) / 1400); draw(); if (anim.a < 1) anim.raf = requestAnimationFrame(step); };
      anim.a = 0; anim.raf = requestAnimationFrame(step);
    };
    const CX = 165, CY = 150, S = 100, FLAT = Math.SQRT1_2;   // the fixed grid is sized for a perfectly flat group: both coordinates 0.71
    const GOLD = 'var(--sx-gold)', BLUE = 'var(--sx-blue)';    // the spiky groups / the flat groups
    const cl = sc.groups === 'clusters', NA = cl ? 'spiky groups' : 'first half (spiky)', NB = cl ? 'flat groups' : 'second half (flat)';
    const svg = $(fig, 'svg.sx-r2'), note = $(fig, '.sx-readout');
    const on = (sel, ev, fn) => { const e = $(fig, sel); if (e) e.addEventListener(ev, fn); };
    const everyone = () => sc.subs.forEach((f) => f());
    const mode = () => (has('quant') ? sc.st.mode : 'plain');      // before quantization has been introduced, show the weights as they are
    const vecs = (grp, md, d) => grp.map((g) => turn2(g.dir, d).map((x) => x * (md === 'plain' ? g.len : 1)));
    const quant = (p, md) => { const m = md === 'plain' ? Math.max(...p.map(Math.abs)) : FLAT, st = 2 * m / 3, ix = p.map((x) => Math.max(0, Math.min(3, Math.round((x + m) / st)))), q = ix.map((k) => -m + k * st); return { m, q, ix, err: Math.hypot(p[0] - q[0], p[1] - q[1]) / Math.hypot(...p) }; };
    // a group's marker: a triangle if it has an outlier, a circle if not
    const shape = (outlier, cx, cy, r, cls, style, extra) => (outlier
      ? sv('polygon', { points: `${cx},${cy - r * 1.3} ${cx - r * 1.2},${cy + r * .85} ${cx + r * 1.2},${cy + r * .85}`, class: cls, ...extra }, style)
      : sv('circle', { cx, cy, r, class: cls, ...extra }, style));
    const inside = (outlier) => (outlier ? 3 : .5);           // where the number sits inside the marker
    const draw = () => {
      const grp = groupsOf(sc), md = mode(), N = grp.length, col = (i) => `var(--pc${i % 8})`; rot.sel = Math.min(rot.sel, N - 1);
      const P = vecs(grp, md, rot.deg), P0 = vecs(grp, md, 0), Q = P.map((p) => quant(p, md));
      const pc = (v) => (v * 100).toFixed(0) + '%', mean = (a) => a.reduce((x, y) => x + y, 0) / (a.length || 1);
      const tw = $(fig, '[data-v=tw]');
      if (tw) {                                          // three lines of cells at the cell size of the grouping figure, growing to the right: weights with their scales, the scales, the rounding errors
        const CW2 = 10, RH2 = 11, BAND = 15, scl = Q.map((o, i) => (md === 'plain' ? o.m : grp[i].len)), smax = Math.max(...scl), line1 = { R: 1, C: 2 * N, CW: CW2, RH: RH2, gs: 2, rows: false };
        const w = (c) => P[c >> 1][c & 1], q = (c) => Q[c >> 1].q[c & 1];
        const chips = P.flatMap((p, i) => { const g = shape(grp[i].spiky, (i + .5) * 2 * CW2, 7, 5.5, '', `fill:${col(i)}`), t = sv('text', { x: (i + .5) * 2 * CW2, y: 7.3 + (grp[i].spiky ? 1.5 : 0), 'text-anchor': 'middle', 'dominant-baseline': 'central', class: 'gnum2' }); t.textContent = i + 1; return [g, t]; });
        const put = (svgEl, h, kids) => { svgEl.setAttribute('viewBox', `0 0 640 ${h}`); svgEl.replaceChildren(...kids); };
        const shifted = sv('g', { transform: `translate(0,${BAND})` }); shifted.append(...matrixCells({ ...line1, color: (r, c) => divColor(w(c), md === 'plain' ? 1.5 : 1), label: (r, g) => scl[g].toFixed(2) }));
        put(tw, BAND + RH2, [...chips, shifted]);
        put($(fig, '[data-v=ts]'), RH2, matrixGroups({ ...line1, color: (r, g) => goldColor(scl[g], smax) }));
        put($(fig, '[data-v=te]'), RH2, matrixCells({ ...line1, color: (r, c) => redColor(Math.abs(w(c) - q(c)), .5) }));
      }
      const lossSvg = $(fig, '[data-v=loss]');
      if (lossSvg) {                                     // how much each group loses, split into groups with an outlier and the rest (like the figure in step 2)
        const kids = [], rows = cl ? [['Spiky groups', 'near an axis', true, 52], ['Flat groups', 'near a diagonal', false, 118]] : [['First half', 'closer to an axis (spiky)', true, 52], ['Second half', 'closer to a diagonal (flat)', false, 118]], top = has('curve') ? .6 : Math.max(...Q.map((o) => o.err)) * 1.2, x0 = 200, x1 = 620, X = (v) => x0 + (x1 - x0) * v / top;
        const word = (x, y, str, anchor, cls, style) => { const t = sv('text', { x, y, 'text-anchor': anchor, class: cls || 'lbl' }, style); t.textContent = str; return t; };
        kids.push(sv('line', { x1: x0, x2: x1, y1: 158, y2: 158, class: 'ax' }));
        const step = top > .5 ? .2 : top > .25 ? .1 : .05;
        for (let v = 0; v <= top; v += step) { kids.push(sv('line', { x1: X(v), x2: X(v), y1: 158, y2: 163, class: 'ax' })); kids.push(word(X(v), 176, pc(v), 'middle')); }
        kids.push(word((x0 + x1) / 2, 194, "rounding error of the group, as a share of its length", 'middle'));
        rows.forEach(([name, sub, flag, y]) => {
          const list = Q.map((o, i) => ({ i, loss: o.err })).filter((g) => grp[g.i].spiky === flag);
          kids.push(word(10, y - 8, name, 'start', 'lbl rowname'), word(10, y + 7, sub, 'start'), word(10, y + 22, `${list.length} group${list.length === 1 ? '' : 's'}`, 'start'));
          list.forEach((g, k) => { const cx = X(g.loss), cy = y + ((k % 3) - 1) * 20; kids.push(shape(flag, cx, cy, 8, 'lossdot', `fill:${col(g.i)}`)); const t = word(cx, cy + inside(flag), g.i + 1, 'middle', 'lbl gnum', 'fill:var(--bg);dominant-baseline:central'); kids.push(t); });
          const mu = mean(list.map((g) => g.loss)); kids.push(sv('line', { x1: X(mu), x2: X(mu), y1: y - 38, y2: y + 38, class: 'mean' }), word(X(mu), y - 42, 'mean ' + pc(mu), 'middle', 'lbl meant'));
        });
        lossSvg.replaceChildren(...kids);
        const ln = $(fig, '[data-v=lossnote]'), mw = mean(Q.filter((o, i) => grp[i].spiky).map((o) => o.err)), mo = mean(Q.filter((o, i) => !grp[i].spiky).map((o) => o.err));
        if (ln) ln.textContent = `${cl ? 'The spiky groups (triangles)' : 'The first half (spiky, triangles)'} lose ${pc(mw)} of their length on average, ${cl ? 'the flat groups (circles)' : 'the second half (flat, circles)'} ${pc(mo)}.${mo < .005 ? ' The flat groups lose almost nothing.' : mw >= mo * 1.05 ? ` The spiky ones lose ${(mw / mo).toFixed(1)} times as much.` : mo >= mw * 1.05 ? ` Now the flat ones lose more: ${(mo / mw).toFixed(1)} times as much.` : ' About the same.'}`;
      }
      if (!svg) return;
      const kids = [], sel = P[rot.sel], { m, err } = Q[rot.sel];
      // Camera off: it stays with the axes and the points move. On: it stays with the points, and everything else turns the other way.
      const pt = (x, y) => { const [a, b] = rot.follow ? turn2([x, y], -rot.deg) : [x, y]; return [CX + S * a, CY - S * b]; };
      const line = (x1, y1, x2, y2, cls, style) => sv('line', { x1, y1, x2, y2, class: cls }, style);
      const seg = (a, b, cls, style) => { const [x1, y1] = pt(...a), [x2, y2] = pt(...b); return line(x1, y1, x2, y2, cls, style); };
      const word = (x, y, str, anchor, cls, style) => { const t = sv('text', { x, y, 'text-anchor': anchor, class: cls || 'lbl' }, style); t.textContent = str; return t; };
      if (has('quant') && md === 'fixed') kids.push(sv('circle', { cx: CX, cy: CY, r: S, class: 'sph' }));
      kids.push(seg([-1.45, 0], [1.45, 0], 'ax'), seg([0, -1.45], [0, 1.45], 'ax'));
      { const [ax, ay] = pt(1.45, 0), [bx, by] = pt(0, 1.45); kids.push(word(ax + 4, ay + 4, 'w₁', 'start', 'lbl axl'), word(bx + 8, by + 12, 'w₂', 'start', 'lbl axl')); }
      if (has('quant')) {
        const square = (mm, style) => sv('polygon', { points: [[-mm, -mm], [mm, -mm], [mm, mm], [-mm, mm]].map(([x, y]) => pt(x, y).join(',')).join(' '), class: 'sq' }, style);
        const grid = (mm, style, r) => { for (let a = 0; a < 4; a++) for (let b = 0; b < 4; b++) { const [cx, cy] = pt(-mm + a * 2 * mm / 3, -mm + b * 2 * mm / 3); kids.push(sv('circle', { cx, cy, r, class: 'gd' }, style)); } };
        if (md === 'plain') Q.forEach((o, i) => {          // every group has its own square, in its own colour
          if (v('squares')) kids.push(square(o.m, `stroke:${col(i)};stroke-width:${i === rot.sel ? 2 : 1};opacity:${i === rot.sel ? 1 : .55}`));   // thin and faint: the dots sit on their squares' edges, so the arrows must stand out from them
          if (v('grid')) grid(o.m, `fill:${col(i)}`, 1.8);
        });
        else { if (v('squares')) kids.push(square(FLAT, 'stroke:var(--sx-gold);stroke-width:1.6')); if (v('grid')) grid(FLAT, 'fill:var(--muted)', 2); }
        if (!has('trail') && v('trail')) P.forEach((p, i) => {   // a curved arrow from each group to the grid point it is stored as: it bows toward the middle, off the square's edge that the dot sits on
          const [x1, y1] = pt(p[0], p[1]), [x2, y2] = pt(Q[i].q[0], Q[i].q[1]), d = Math.hypot(x2 - x1, y2 - y1); if (d < 22) return;
          const mx = (x1 + x2) / 2, my = (y1 + y2) / 2; let nx = -(y2 - y1) / d, ny = (x2 - x1) / d; if (nx * (CX - mx) + ny * (CY - my) < 0) { nx = -nx; ny = -ny; }
          const bow = Math.min(.45 * d, 22), cx = mx + nx * bow * 2, cy = my + ny * bow * 2;      // the control point of a quadratic curve sits twice as far out as the curve's middle
          const n0 = Math.hypot(cx - x1, cy - y1), u0x = (cx - x1) / n0, u0y = (cy - y1) / n0, n1 = Math.hypot(x2 - cx, y2 - cy), u1x = (x2 - cx) / n1, u1y = (y2 - cy) / n1;
          const sx = x1 + u0x * 9, sy = y1 + u0y * 9, tx = x2 - u1x * 9, ty = y2 - u1y * 9, hl = Math.min(11, d * .35), bx = tx - u1x * hl, by = ty - u1y * hl, wd = i === rot.sel ? 3 : 2.2, hw = hl * .45;
          const tri = `${tx},${ty} ${bx - u1y * hw},${by + u1x * hw} ${bx + u1y * hw},${by - u1x * hw}`, path = `M${sx},${sy} Q${cx},${cy} ${bx},${by}`;
          kids.push(sv('path', { d: path, class: 'arc' }, `stroke:var(--bg);stroke-width:${wd + 3.5}`), sv('polygon', { points: tri, class: 'arrh' }, 'fill:var(--bg);stroke:var(--bg);stroke-width:3.5;stroke-linejoin:round'));   // a background-coloured casing, so the arrow reads as lying on top of anything under it
          kids.push(sv('path', { d: path, class: 'arc' }, `stroke:${col(i)};stroke-width:${wd}`), sv('polygon', { points: tri, class: 'arrh' }, `fill:${col(i)}`));
        });
      }
      const bin = (k) => k.toString(2).padStart(2, '0'), obstacles = [];     // boxes that labels must keep clear of: markers and level numbers
      if (has('bits') && v('bits')) {                    // each grid point is a possible stored value: its level numbers, in binary, are what is saved
        const o = Q[rot.sel], st = 2 * o.m / 3;
        for (let k = 0; k < 4; k++) { const lv = -o.m + k * st, [bx, by] = pt(lv, -o.m), [lx, ly] = pt(-o.m, lv); kids.push(word(bx, by + 14, bin(k), 'middle', 'lbl bitk', `fill:${col(rot.sel)}`), word(lx - 6, ly + 3.5, bin(k), 'end', 'lbl bitk', `fill:${col(rot.sel)}`)); obstacles.push({ x: bx - 9, y: by + 4, w: 18, h: 12 }, { x: lx - 24, y: ly - 6, w: 20, h: 12 }); }
      }
      if (has('quant') && !has('trail') && v('end')) Q.forEach((o, i) => {   // a hollow marker where each group is stored: the dot moved there
        const [x, y] = pt(o.q[0], o.q[1]), [px, py] = pt(P[i][0], P[i][1]);
        kids.push(shape(grp[i].spiky, x, y, 8, 'dest', `stroke:${col(i)};stroke-width:${i === rot.sel ? 3 : 2}`));
        if (Math.hypot(x - px, y - py) >= 22) kids.push(word(x, y + inside(grp[i].spiky), i + 1, 'middle', 'lbl gnum', `fill:${col(i)};dominant-baseline:central`));
      });
      if (has('ghost') && v('pre')) P0.forEach((p, i) => { const [cx, cy] = pt(p[0], p[1]); kids.push(shape(grp[i].spiky, cx, cy, 7, 'ghost', `stroke:${col(i)}`)); });   // where the groups started, relative to the axes
      const trail = has('trail'), e = trail ? ease(anim.a) : 1;   // with a trail, each group is drawn once, at its place on the way from its real weights to where it is stored, with fading shadows behind it
      if (trail) P.forEach((p, i) => {
        const [x1, y1] = pt(p[0], p[1]), [x2, y2] = pt(Q[i].q[0], Q[i].q[1]), d = Math.hypot(x2 - x1, y2 - y1) * e; if (d < 4) return;
        const n = Math.max(3, Math.min(7, Math.round(d / 8)));
        for (let k = 0; k < n; k++) { if (!(k === 0 ? v('start') : v('trail'))) continue; const t = k / n; kids.push(shape(grp[i].spiky, x1 + (x2 - x1) * e * t, y1 + (y2 - y1) * e * t, 8, 'shadow', `fill:${col(i)};opacity:${(.12 + .5 * t).toFixed(2)}`)); }   // the faintest one is where the group really is
      });
      const at = (p, i) => (trail ? [p[0] + (Q[i].q[0] - p[0]) * e, p[1] + (Q[i].q[1] - p[1]) * e] : p);
      const showMarker = trail ? v('end') : v('start');   // the solid marker is where a group is stored (trail figure) or where it is after the rotation (the others)
      if (showMarker) P.forEach((p, i) => { const [cx, cy] = pt(...at(p, i)); kids.push(shape(grp[i].spiky, cx, cy, 8, 'pt' + (i === rot.sel ? ' sel' : ''), `fill:${col(i)}`, { 'data-i': i })); });
      if (showMarker) P.forEach((p, i) => { const [cx, cy] = pt(...at(p, i)); kids.push(word(cx, cy + inside(grp[i].spiky), i + 1, 'middle', 'lbl gnum', 'fill:var(--bg);dominant-baseline:central')); });   // the group's number, inside its marker
      if (has('bits') && v('bits')) {                    // the stored value of each group, kept clear of every marker, of the level numbers and of the other labels
        Q.forEach((o, i) => { const [x, y] = pt(o.q[0], o.q[1]); obstacles.push({ x: x - 11, y: y - 11, w: 22, h: 22 }); });
        const overlap = (a, b) => Math.max(0, Math.min(a.x + a.w, b.x + b.w) - Math.max(a.x, b.x)) * Math.max(0, Math.min(a.y + a.h, b.y + b.h) - Math.max(a.y, b.y));
        const VX0 = 0, VX1 = 330, VY0 = -8, VY1 = 314, placed = [];
        Q.forEach((o, i) => {
          const [x, y] = pt(o.q[0], o.q[1]), text = `(${o.ix.map(bin).join(',')})`, w = text.length * 6.2 + 2, h = 12;
          let best = null;
          for (const dist of [13, 21, 31]) for (const [dx, dy] of [[1, 1], [-1, 1], [1, -1], [-1, -1], [1, 0], [-1, 0], [0, 1], [0, -1]]) {
            const anchor = dx > 0 ? 'start' : dx < 0 ? 'end' : 'middle', tx = x + dx * dist * (dy ? .8 : 1), ty = y + dy * dist * (dx ? .8 : 1) + (dy > 0 ? 6 : dy < 0 ? -2 : 4);   // the text's baseline
            const bx = anchor === 'start' ? tx : anchor === 'end' ? tx - w : tx - w / 2, box = { x: bx, y: ty - 9.5, w, h };
            const out = Math.max(0, VX0 - box.x) + Math.max(0, box.x + w - VX1) + Math.max(0, VY0 - box.y) + Math.max(0, box.y + h - VY1);
            const score = [...obstacles, ...placed].reduce((a, ob) => a + overlap(box, ob), 0) + out * 40 + dist * .3;
            if (!best || score < best.score) best = { score, tx, ty, anchor, box };
          }
          placed.push(best.box);
          kids.push(word(best.tx, best.ty, text, best.anchor, 'lbl bitl', `fill:${col(i)};opacity:${(e ** 3).toFixed(2)}`));
        });
      }
      let curves = null;
      if (has('curve')) {                                // the largest weight as a share of the group's length, at every angle: one curve for each kind of group
        const spike = (flag) => (d) => { const g2 = grp.filter((g) => g.spiky === flag); return mean(g2.map((g) => { const q = turn2(g.dir, d); return Math.max(Math.abs(q[0]), Math.abs(q[1])); })); };
        const cW = Array.from({ length: 91 }, (_, d) => spike(true)(d)), cO = Array.from({ length: 91 }, (_, d) => spike(false)(d)); curves = { cW, cO };
        const x0 = 410, x1 = 620, y0 = 250, y1 = 50, lo = .65, top = 1.02, CXa = (d) => x0 + (x1 - x0) * d / 90, CYa = (e) => y0 - (y0 - y1) * (e - lo) / (top - lo);
        kids.push(line(x0, y0, x1, y0, 'ax'), line(x0, y0, x0, y1, 'ax'));
        if (had) kids.push(line(CXa(45), y0, CXa(45), y1, 'hadl'));
        [[cW, GOLD], [cO, BLUE]].forEach(([c, color]) => { kids.push(sv('polyline', { points: c.map((e, d) => `${CXa(d)},${CYa(e)}`).join(' '), class: 'crv' }, `stroke:${color}`), sv('circle', { cx: CXa(rot.deg), cy: CYa(c[rot.deg]), r: 4.5, class: 'cdot' }, `fill:${color}`)); });
        kids.push(line(CXa(rot.deg), y0, CXa(rot.deg), y1, 'cur'));
        (had ? [0, 45, 90] : [0, 90]).forEach((d) => kids.push(word(CXa(d), y0 + 15, d + '°', 'middle')));
        [.7, .85, 1].forEach((v) => kids.push(line(x0 - 4, CYa(v), x0, CYa(v), 'ax'), word(x0 - 8, CYa(v) + 4, (v * 100).toFixed(0) + '%', 'end')));
        if (had) kids.push(word(CXa(45), y0 + 28, 'Hadamard', 'middle', 'lbl hadt'));
        kids.push(shape(true, x0 - 32, 32, 6, 'ghost', `stroke:${GOLD};stroke-dasharray:none`), word(x0 - 22, 36, NA, 'start'), shape(false, x0 + 96, 32, 6, 'ghost', `stroke:${BLUE};stroke-dasharray:none`), word(x0 + 106, 36, NB, 'start'));   // the legend sits above the plot, clear of the curves
        kids.push(word(x0 - 40, y1 - 38, 'largest weight ÷ length (average)', 'start'), word((x0 + x1) / 2, y0 + (had ? 44 : 30), 'rotation angle', 'middle'));
      }
      svg.replaceChildren(...kids);
      if (!note || !curves) return;
      const avgErr = (flag) => (d) => mean(grp.filter((g) => g.spiky === flag).map((g) => quant(turn2(g.dir, d).map((x) => x * (md === 'plain' ? g.len : 1)), md).err));
      const set = (flag, name, c) => `${name}: largest weight ${pc(c[rot.deg])} of the length (${pc(c[0])} before${had ? `, ${pc(c[45])} at 45°` : ''}), rounding error ${pc(avgErr(flag)(rot.deg))} (${pc(avgErr(flag)(0))} before${had ? `, ${pc(avgErr(flag)(45))} at 45°` : ''})`;
      note.textContent = `Rotated ${rot.deg}°. Group ${rot.sel + 1} (${grp[rot.sel].spiky ? 'spiky' : 'flat'}): (${sel[0].toFixed(2)}, ${sel[1].toFixed(2)}), largest weight ${pc(Math.max(...sel.map(Math.abs)) / Math.hypot(...sel))} of its length, rounding error ${pc(err)}. On average, ${set(true, cl ? 'spiky groups (triangles)' : 'first half (spiky, triangles)', curves.cW)}; ${set(false, cl ? 'flat groups (circles)' : 'second half (flat, circles)', curves.cO)}. ${md === 'plain' ? 'Each group has its own square, reaching its largest weight.' : 'Every group shares the one fixed grid.'}`;
      const box = $(fig, '.sx-mat');
      if (box) {
        const c = Math.cos(rot.deg * Math.PI / 180), sn = Math.sin(rot.deg * Math.PI / 180), h = Math.SQRT1_2;
        const tbl = (M) => { const t = el('table'); M.forEach((row) => { const tr = el('tr'); row.forEach((v) => { const td = el('td', 'num', (v < 0 ? '−' : '') + Math.abs(v).toFixed(2)); td.style.background = `color-mix(in srgb, var(--sx-gold) ${Math.abs(v) * 70}%, transparent)`; tr.append(td); }); t.append(tr); }); return t; };
        const lab = (t) => el('span', 'sx-note', t);
        box.replaceChildren(lab('rotation by this angle'), tbl([[c, -sn], [sn, c]]), lab('Hadamard matrix'), tbl([[h, h], [h, -h]]), lab(rot.deg === 45 ? 'At 45° the rotation has the same entries as the Hadamard matrix, with its two rows swapped.' : ''));
      }
    };
    if (fig.dataset.toggles && svg) {                   // a row of switches for the elements of the plot, e.g. data-toggles="squares:Squares;grid:Grid points"
      const row = el('div', 'sx-controls'); row.append(el('span', 'sx-rowlabel', 'Show'));
      fig.dataset.toggles.split(';').forEach((tok) => { const [k, label] = tok.split(':'), b = el('button', null, label); b.type = 'button'; b.setAttribute('aria-pressed', 'true'); b.addEventListener('click', () => { vis[k] = !v(k); b.setAttribute('aria-pressed', String(v(k))); draw(); }); row.append(b); });
      svg.before(row);
    }
    sc.subs.push(draw);
    // mock groups and levels: shared by every figure of the scene
    on('[data-k=n]', 'input', (e) => { sc.st.n = +e.target.value; $(fig, '[data-o=n]').textContent = sc.st.n; everyone(); });
    fig.querySelectorAll('[data-mode]').forEach((b) => b.addEventListener('click', () => { sc.st.mode = b.dataset.mode; fig.querySelectorAll('[data-mode]').forEach((x) => x.setAttribute('aria-pressed', String(x === b))); everyone(); }));
    // rotation: per figure
    const set = (d) => { rot.deg = d; $(fig, '[data-k=deg]').value = d; $(fig, '[data-o=deg]').textContent = d + '°'; draw(); };
    on('[data-k=deg]', 'input', (e) => set(+e.target.value));
    on('[data-act=had]', 'click', () => set(45));
    on('[data-act=reset]', 'click', () => set(0));
    on('[data-act=replay]', 'click', play);
    on('[data-act=follow]', 'click', (e) => { rot.follow = !rot.follow; e.currentTarget.setAttribute('aria-pressed', String(rot.follow)); draw(); });
    if (svg) svg.addEventListener('click', (e) => { const i = e.target.dataset && e.target.dataset.i; if (i != null) { rot.sel = +i; draw(); } });
    { const n = $(fig, '[data-o=n]'); if (n) n.textContent = sc.st.n; }
    if ($(fig, '[data-k=deg]')) set(0); else draw();
    if (has('trail') && svg && 'IntersectionObserver' in window) new IntersectionObserver((es, ob) => { if (es[0].isIntersecting) { ob.disconnect(); play(); } }, { threshold: .6 }).observe(svg);   // play once when the figure is first in view
  };

  // 6a. Groups of three weights: the two-weight figures in 3D. Figures with the same data-scene share their groups; data-layers says what each draws:
  // strips, points, quant (cubes, grids, rings and arrows), ghost (where the groups started), curve (spikiness against angle), loss (how much each group loses).
  // The groups run from a short one dominated by one weight (on an axis) to a long one with three equal weights (on the diagonal (1, 1, 1)), spread evenly in length and direction;
  // the first half are spiky (triangles), the second half flat (circles). The rotation is a turn about that diagonal: 60 degrees is the flattest 3x3 rotation.
  const scenes3 = {};
  const cross3 = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const norm3 = (v) => { const l = Math.hypot(...v); return v.map((x) => x / l); };
  const rodr = (v, ax, ang) => { const c = Math.cos(ang), s = Math.sin(ang), d = ax[0] * v[0] + ax[1] * v[1] + ax[2] * v[2], cr = cross3(ax, v); return [0, 1, 2].map((i) => v[i] * c + cr[i] * s + ax[i] * d * (1 - c)); };
  const DIAG3 = norm3([1, 1, 1]), TILT = Math.acos(1 / Math.sqrt(3)), TILT_AX = norm3(cross3([1, 0, 0], DIAG3));
  const groups3 = (n) => Array.from({ length: n }, (_, i) => { const t = n > 1 ? i / (n - 1) : 0; return { dir: rodr([1, 0, 0], TILT_AX, TILT * t), len: .35 + t, spiky: i < n / 2 }; });
  const turn3 = (v, deg) => rodr(v, DIAG3, deg * Math.PI / 180);
  const quant3 = (p) => { const m = Math.max(...p.map(Math.abs)), st = 2 * m / 3, ix = p.map((x) => Math.max(0, Math.min(3, Math.round((x + m) / st)))), q = ix.map((k) => -m + k * st); return { m, q, ix, err: Math.hypot(p[0] - q[0], p[1] - q[1], p[2] - q[2]) / Math.hypot(...p) }; };
  const marker3 = (spiky, cx, cy, r, cls, style, extra) => (spiky
    ? sv('polygon', { points: `${cx},${cy - r * 1.3} ${cx - r * 1.2},${cy + r * .85} ${cx + r * 1.2},${cy + r * .85}`, class: cls, ...extra }, style)
    : sv('circle', { cx, cy, r, class: cls, ...extra }, style));
  const rot3dgFig = (fig) => {
    const layers = (fig.dataset.layers || 'points').split(/\s+/), has = (l) => layers.includes(l);
    const sc = scenes3[fig.dataset.scene || 'three'] || (scenes3[fig.dataset.scene || 'three'] = { st: { n: 4 }, subs: [] });
    const rot = { deg: 0, sel: 0, follow: false }, view = { yaw: -.6, pitch: .4 }, vis = {}, v = (k) => vis[k] !== false;
    const CX = 190, CY = 170, S = 100, GOLD = 'var(--sx-gold)', BLUE = 'var(--sx-blue)';
    const svg = $(fig, 'svg.sx-r3g'), note = $(fig, '.sx-readout');
    const on = (sel, ev, fn) => { const e = $(fig, sel); if (e) e.addEventListener(ev, fn); };
    const mean = (a) => a.reduce((x, y) => x + y, 0) / (a.length || 1), pc = (x) => (x * 100).toFixed(0) + '%';
    const word = (x, y, str, anchor, cls, style) => { const t = sv('text', { x, y, 'text-anchor': anchor, class: cls || 'lbl' }, style); t.textContent = str; return t; };
    let drag = null;
    const draw = () => {
      const grp = groups3(sc.st.n), N = grp.length, col = (i) => `var(--pc${i % 8})`; rot.sel = Math.min(rot.sel, N - 1);
      const P0 = grp.map((g) => g.dir.map((x) => x * g.len)), P = P0.map((p) => turn3(p, rot.deg)), Q = P.map(quant3), inside = (sp) => (sp ? 3 : .5);
      const tw = $(fig, '[data-v=tw]');
      if (tw) {                                          // a row of weights cut into groups of three, with the scales and the rounding errors, at the cell size of the other figures
        const CW = 10, RH = 11, BAND = 15, scl = Q.map((o) => o.m), smax = Math.max(...scl), line1 = { R: 1, C: 3 * N, CW, RH, gs: 3, rows: false };
        const w = (c) => P[(c / 3) | 0][c % 3], q = (c) => Q[(c / 3) | 0].q[c % 3];
        const chips = P.flatMap((p, i) => { const g = marker3(grp[i].spiky, (i + .5) * 3 * CW, 7, 5.5, '', `fill:${col(i)}`), t = sv('text', { x: (i + .5) * 3 * CW, y: 7.3 + (grp[i].spiky ? 1.5 : 0), 'text-anchor': 'middle', 'dominant-baseline': 'central', class: 'gnum2' }); t.textContent = i + 1; return [g, t]; });
        const put = (e, h, kids) => { e.setAttribute('viewBox', `0 0 640 ${h}`); e.replaceChildren(...kids); };
        const shifted = sv('g', { transform: `translate(0,${BAND})` }); shifted.append(...matrixCells({ ...line1, color: (r, c) => divColor(w(c), 1.5), label: (r, g) => scl[g].toFixed(2) }));
        put(tw, BAND + RH, [...chips, shifted]);
        put($(fig, '[data-v=ts]'), RH, matrixGroups({ ...line1, color: (r, g) => goldColor(scl[g], smax) }));
        put($(fig, '[data-v=te]'), RH, matrixCells({ ...line1, color: (r, c) => redColor(Math.abs(w(c) - q(c)), .5) }));
      }
      const lossSvg = $(fig, '[data-v=loss]');
      if (lossSvg) {                                     // how much each group loses, first half against second half
        const kids = [], rows = [['First half', 'closer to an axis (spiky)', true, 52], ['Second half', 'closer to the diagonal (flat)', false, 118]], top = has('curve') ? .6 : Math.max(...Q.map((o) => o.err)) * 1.2, x0 = 200, x1 = 620, X = (x) => x0 + (x1 - x0) * x / top;
        kids.push(sv('line', { x1: x0, x2: x1, y1: 158, y2: 158, class: 'ax' }));
        const step = top > .5 ? .2 : top > .25 ? .1 : .05;
        for (let x = 0; x <= top; x += step) { kids.push(sv('line', { x1: X(x), x2: X(x), y1: 158, y2: 163, class: 'ax' })); kids.push(word(X(x), 176, pc(x), 'middle')); }
        kids.push(word((x0 + x1) / 2, 194, 'rounding error of the group, as a share of its length', 'middle'));
        rows.forEach(([name, sub, flag, y]) => {
          const list = Q.map((o, i) => ({ i, loss: o.err })).filter((g) => grp[g.i].spiky === flag);
          kids.push(word(10, y - 8, name, 'start', 'lbl rowname'), word(10, y + 7, sub, 'start'), word(10, y + 22, `${list.length} group${list.length === 1 ? '' : 's'}`, 'start'));
          list.forEach((g, k) => { const cx = X(g.loss), cy = y + ((k % 3) - 1) * 20; kids.push(marker3(flag, cx, cy, 8, 'lossdot', `fill:${col(g.i)}`), word(cx, cy + inside(flag), g.i + 1, 'middle', 'lbl gnum', 'fill:var(--bg);dominant-baseline:central')); });
          const mu = mean(list.map((g) => g.loss)); kids.push(sv('line', { x1: X(mu), x2: X(mu), y1: y - 38, y2: y + 38, class: 'mean' }), word(X(mu), y - 42, 'mean ' + pc(mu), 'middle', 'lbl meant'));
        });
        lossSvg.replaceChildren(...kids);
        const ln = $(fig, '[data-v=lossnote]'), mw = mean(Q.filter((o, i) => grp[i].spiky).map((o) => o.err)), mo = mean(Q.filter((o, i) => !grp[i].spiky).map((o) => o.err));
        if (ln) ln.textContent = `The first half (spiky, triangles) lose ${pc(mw)} of their length on average, the second half (flat, circles) ${pc(mo)}.${mo < .005 ? ' The flat groups lose almost nothing.' : mw >= mo * 1.05 ? ` The spiky ones lose ${(mw / mo).toFixed(1)} times as much.` : mo >= mw * 1.05 ? ` Now the flat ones lose more: ${(mo / mw).toFixed(1)} times as much.` : ' About the same.'}`;
      }
      if (!svg) return;
      const kids = [], sel = P[rot.sel], { m, err } = Q[rot.sel];
      // Camera off: it stays with the axes and the points move. On: it stays with the points, and everything else turns the other way.
      const proj = (p0) => { const p = rot.follow ? turn3(p0, -rot.deg) : p0, cy = Math.cos(view.yaw), sy = Math.sin(view.yaw), cp = Math.cos(view.pitch), sp = Math.sin(view.pitch), x1 = p[0] * cy - p[1] * sy, y1 = p[0] * sy + p[1] * cy; return [CX + S * x1, CY - S * (p[2] * cp + y1 * sp), y1 * cp - p[2] * sp]; };
      const seg = (a, b, cls, style) => { const [x1, y1] = proj(a), [x2, y2] = proj(b); return sv('line', { x1, y1, x2, y2, class: cls }, style); };
      [[1.5, 0, 0, 'w₁'], [0, 1.5, 0, 'w₂'], [0, 0, 1.5, 'w₃']].forEach(([x, y, z, t]) => { kids.push(seg([-x, -y, -z], [x, y, z], 'ax')); const [px, py] = proj([x * 1.09, y * 1.09, z * 1.09]); kids.push(word(px, py + 4, t, 'middle', 'lbl axl')); });
      if (has('quant')) {
        const edges = [[0, 1], [1, 2], [2, 3], [3, 0], [4, 5], [5, 6], [6, 7], [7, 4], [0, 4], [1, 5], [2, 6], [3, 7]];
        Q.forEach((o, i) => {                            // every group has its own cube of levels, in its own colour
          const mm = o.m, C8 = [[-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1], [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1]].map((c) => c.map((x) => x * mm));
          if (v('cubes')) edges.forEach(([a, b]) => kids.push(seg(C8[a], C8[b], 'cube3', `stroke:${col(i)};stroke-width:${i === rot.sel ? 1.8 : 1};opacity:${i === rot.sel ? 1 : .45}`)));
          if (v('grid')) for (let a = 0; a < 4; a++) for (let b = 0; b < 4; b++) for (let c = 0; c < 4; c++) { const [x, y] = proj([-mm + a * 2 * mm / 3, -mm + b * 2 * mm / 3, -mm + c * 2 * mm / 3]); kids.push(sv('circle', { cx: x, cy: y, r: 1.3, class: 'gd' }, `fill:${col(i)};opacity:.45`)); }
        });
        if (v('end')) Q.forEach((o, i) => {              // a hollow marker where each group is stored
          const [x, y] = proj(o.q), [px, py] = proj(P[i]);
          kids.push(marker3(grp[i].spiky, x, y, 8, 'dest', `stroke:${col(i)};stroke-width:${i === rot.sel ? 3 : 2}`));
          if (Math.hypot(x - px, y - py) >= 22) kids.push(word(x, y + inside(grp[i].spiky), i + 1, 'middle', 'lbl gnum', `fill:${col(i)};dominant-baseline:central`));
        });
        if (v('trail')) P.forEach((p, i) => {            // an arrow from each group to the grid point it is stored as
          const [x1, y1] = proj(p), [x2, y2] = proj(Q[i].q), d = Math.hypot(x2 - x1, y2 - y1); if (d < 22) return;
          const ux = (x2 - x1) / d, uy = (y2 - y1) / d, sx = x1 + ux * 9, sy = y1 + uy * 9, tx = x2 - ux * 9, ty = y2 - uy * 9, hl = Math.min(11, (d - 18) * .7), bx = tx - ux * hl, by = ty - uy * hl, wd = i === rot.sel ? 3 : 2.2, hw = hl * .45;
          const tri = `${tx},${ty} ${bx - uy * hw},${by + ux * hw} ${bx + uy * hw},${by - ux * hw}`;
          kids.push(sv('line', { x1: sx, y1: sy, x2: bx, y2: by, class: 'snap3' }, `stroke:var(--bg);stroke-width:${wd + 3.5}`), sv('polygon', { points: tri, class: 'arrh' }, 'fill:var(--bg);stroke:var(--bg);stroke-width:3.5;stroke-linejoin:round'));
          kids.push(sv('line', { x1: sx, y1: sy, x2: bx, y2: by, class: 'snap3' }, `stroke:${col(i)};stroke-width:${wd}`), sv('polygon', { points: tri, class: 'arrh' }, `fill:${col(i)}`));
        });
      }
      if (has('ghost') && v('pre')) P0.forEach((p, i) => { const [cx, cy] = proj(p); kids.push(marker3(grp[i].spiky, cx, cy, 7, 'ghost', `stroke:${col(i)}`)); });   // where the groups started, relative to the axes
      if (v('start')) P.map((p, i) => ({ i, pr: proj(p) })).sort((a, b) => b.pr[2] - a.pr[2]).forEach(({ i, pr }) => {   // far ones first
        kids.push(marker3(grp[i].spiky, pr[0], pr[1], 8, 'pt' + (i === rot.sel ? ' sel' : ''), `fill:${col(i)}`, { 'data-i': i }), word(pr[0], pr[1] + inside(grp[i].spiky), i + 1, 'middle', 'lbl gnum', 'fill:var(--bg);dominant-baseline:central'));
      });
      let curves = null;
      if (has('curve')) {                                // the largest weight as a share of the group's length, at every angle, for each half
        const spike = (flag) => (d) => mean(grp.filter((g) => g.spiky === flag).map((g) => Math.max(...turn3(g.dir, d).map(Math.abs))));
        const cW = Array.from({ length: 121 }, (_, d) => spike(true)(d)), cO = Array.from({ length: 121 }, (_, d) => spike(false)(d)); curves = { cW, cO };
        const x0 = 410, x1 = 620, y0 = 250, y1 = 50, lo = .5, top = 1.02, CXa = (d) => x0 + (x1 - x0) * d / 120, CYa = (e) => y0 - (y0 - y1) * (e - lo) / (top - lo);
        kids.push(sv('line', { x1: x0, x2: x1, y1: y0, y2: y0, class: 'ax' }), sv('line', { x1: x0, x2: x0, y1: y0, y2: y1, class: 'ax' }), sv('line', { x1: CXa(60), x2: CXa(60), y1: y0, y2: y1, class: 'hadl' }));
        [[cW, GOLD], [cO, BLUE]].forEach(([c, color]) => kids.push(sv('polyline', { points: c.map((e, d) => `${CXa(d)},${CYa(e)}`).join(' '), class: 'crv' }, `stroke:${color}`), sv('circle', { cx: CXa(rot.deg), cy: CYa(c[rot.deg]), r: 4.5, class: 'cdot' }, `fill:${color}`)));
        kids.push(sv('line', { x1: CXa(rot.deg), x2: CXa(rot.deg), y1: y0, y2: y1, class: 'cur' }));
        [0, 60, 120].forEach((d) => kids.push(word(CXa(d), y0 + 15, d + '°', 'middle')));
        [.6, .8, 1].forEach((x) => kids.push(sv('line', { x1: x0 - 4, x2: x0, y1: CYa(x), y2: CYa(x), class: 'ax' }), word(x0 - 8, CYa(x) + 4, pc(x), 'end')));
        kids.push(marker3(true, x0 - 32, 32, 6, 'ghost', `stroke:${GOLD};stroke-dasharray:none`), word(x0 - 22, 36, 'first half (spiky)', 'start'), marker3(false, x0 + 96, 32, 6, 'ghost', `stroke:${BLUE};stroke-dasharray:none`), word(x0 + 106, 36, 'second half (flat)', 'start'));
        kids.push(word(x0 - 40, y1 - 38, 'largest weight ÷ length (average)', 'start'), word((x0 + x1) / 2, y0 + 30, 'rotation angle', 'middle'));
      }
      svg.replaceChildren(...kids);
      if (note && curves) {
        const avgErr = (flag) => (d) => mean(grp.filter((g) => g.spiky === flag).map((g) => quant3(turn3(g.dir.map((x) => x * g.len), d)).err));
        const set = (flag, name, c) => `${name}: largest weight ${pc(c[rot.deg])} of the length (${pc(c[0])} before), rounding error ${pc(avgErr(flag)(rot.deg))} (${pc(avgErr(flag)(0))} before)`;
        note.textContent = `Rotated ${rot.deg}°. Group ${rot.sel + 1} (${grp[rot.sel].spiky ? 'spiky' : 'flat'}): largest weight ${pc(Math.max(...sel.map(Math.abs)) / Math.hypot(...sel))} of its length, rounding error ${pc(err)}. On average, ${set(true, 'first half (spiky, triangles)', curves.cW)}; ${set(false, 'second half (flat, circles)', curves.cO)}.`;
      }
      const box = $(fig, '.sx-mat');
      if (box) {                                         // the rotation matrix at this angle: its columns are where the three axes go
        const cols = [[1, 0, 0], [0, 1, 0], [0, 0, 1]].map((e) => turn3(e, rot.deg)), M = [0, 1, 2].map((i) => [0, 1, 2].map((j) => cols[j][i])), t = el('table');
        M.forEach((row) => { const tr = el('tr'); row.forEach((x) => { const td = el('td', 'num', (x < 0 ? '−' : '') + Math.abs(x).toFixed(2)); td.style.background = `color-mix(in srgb, var(--sx-gold) ${Math.abs(x) * 70}%, transparent)`; tr.append(td); }); t.append(tr); });
        const big = Math.max(...M.flat().map(Math.abs)); box.replaceChildren(t, el('p', 'sx-note', `Rotation matrix now: largest entry ${big.toFixed(2)}${rot.deg === 60 ? ', the smallest any 3×3 rotation can reach' : ''}.`));
      }
    };
    const everyone = () => sc.subs.forEach((f) => f());
    if (fig.dataset.toggles && svg) {                  // a row of switches for the elements of the plot
      const row = el('div', 'sx-controls'); row.append(el('span', 'sx-rowlabel', 'Show'));
      fig.dataset.toggles.split(';').forEach((tok) => { const [k, label] = tok.split(':'), b = el('button', null, label); b.type = 'button'; b.setAttribute('aria-pressed', 'true'); b.addEventListener('click', () => { vis[k] = !v(k); b.setAttribute('aria-pressed', String(v(k))); draw(); }); row.append(b); });
      svg.before(row);
    }
    sc.subs.push(draw);
    on('[data-k=n]', 'input', (e) => { sc.st.n = +e.target.value; $(fig, '[data-o=n]').textContent = sc.st.n; everyone(); });
    const set = (d) => { rot.deg = d; $(fig, '[data-k=deg]').value = d; $(fig, '[data-o=deg]').textContent = d + '°'; draw(); };
    on('[data-k=deg]', 'input', (e) => set(+e.target.value));
    on('[data-act=flat]', 'click', () => set(60));
    on('[data-act=reset]', 'click', () => set(0));
    on('[data-act=follow]', 'click', (e) => { rot.follow = !rot.follow; e.currentTarget.setAttribute('aria-pressed', String(rot.follow)); draw(); });
    if (svg) {
      svg.addEventListener('pointerdown', (e) => { drag = { x: e.clientX, y: e.clientY, yaw: view.yaw, pitch: view.pitch, moved: false }; });
      window.addEventListener('pointermove', (e) => { if (!drag) return; const dx = e.clientX - drag.x, dy = e.clientY - drag.y; if (Math.abs(dx) + Math.abs(dy) > 3) drag.moved = true; if (drag.moved) { view.yaw = drag.yaw + dx * .01; view.pitch = Math.max(.05, Math.min(1.4, drag.pitch + dy * .01)); draw(); } });
      window.addEventListener('pointerup', () => { setTimeout(() => { drag = null; }, 0); });
      svg.addEventListener('click', (e) => { const i = e.target.dataset && e.target.dataset.i; if (i != null && !(drag && drag.moved)) { rot.sel = +i; draw(); } });
      if ($(fig, '[data-act=spin]')) spin(fig, view, draw, () => !!drag);
    }
    { const n = $(fig, '[data-o=n]'); if (n) n.textContent = sc.st.n; }
    if ($(fig, '[data-k=deg]')) set(0); else draw();
  };

  // 6c. PCA versus a flattening rotation on a 3D cloud
  const pca3dFig = (fig) => {
    const N = 60, S = 92, CX = 320, CY = 168, state = { ds: 'axes', rot: 'pca', t: 0, yaw: -0.6, pitch: 0.4 }; let seed = 9;
    const rng = (sd) => () => (sd = (sd * 16807) % 2147483647) / 2147483647;
    const svg = $(fig, 'svg'), note = $(fig, '.sx-note'), NS = 'http://www.w3.org/2000/svg';
    const mk = (tag, attrs) => { const e = document.createElementNS(NS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); return e; };
    const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
    const rodrigues = (v, ax, ang) => { const c = Math.cos(ang), s = Math.sin(ang), d = ax[0] * v[0] + ax[1] * v[1] + ax[2] * v[2], cr = cross(ax, v); return [0, 1, 2].map((i) => v[i] * c + cr[i] * s + ax[i] * d * (1 - c)); };
    const quat = (R) => {                               // rotation matrix to quaternion, then to axis and angle (shortest way round)
      const tr = R[0][0] + R[1][1] + R[2][2]; let w, x, y, z;
      if (tr > 0) { const k = Math.sqrt(tr + 1) * 2; w = k / 4; x = (R[2][1] - R[1][2]) / k; y = (R[0][2] - R[2][0]) / k; z = (R[1][0] - R[0][1]) / k; }
      else if (R[0][0] > R[1][1] && R[0][0] > R[2][2]) { const k = Math.sqrt(1 + R[0][0] - R[1][1] - R[2][2]) * 2; w = (R[2][1] - R[1][2]) / k; x = k / 4; y = (R[0][1] + R[1][0]) / k; z = (R[0][2] + R[2][0]) / k; }
      else if (R[1][1] > R[2][2]) { const k = Math.sqrt(1 + R[1][1] - R[0][0] - R[2][2]) * 2; w = (R[0][2] - R[2][0]) / k; x = (R[0][1] + R[1][0]) / k; y = k / 4; z = (R[1][2] + R[2][1]) / k; }
      else { const k = Math.sqrt(1 + R[2][2] - R[0][0] - R[1][1]) * 2; w = (R[1][0] - R[0][1]) / k; x = (R[0][2] + R[2][0]) / k; y = (R[1][2] + R[2][1]) / k; z = k / 4; }
      if (w < 0) { w = -w; x = -x; y = -y; z = -z; }
      const ang = 2 * Math.acos(Math.min(1, w)), sh = Math.sin(ang / 2);
      return sh < 1e-9 ? { ax: [0, 0, 1], ang: 0 } : { ax: [x / sh, y / sh, z / sh], ang };
    };
    let pts = [], pc = null;
    const gen = () => {
      const r = rng(seed), g = () => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r());
      if (state.ds === 'axes') pts = Array.from({ length: N }, () => { const p = [.07 * g(), .07 * g(), .07 * g()]; p[Math.floor(r() * 3)] += (r() < .5 ? -1 : 1) * (.9 + .12 * g()); return p; });
      else { const d = [.8, .45, .35], l = Math.hypot(...d); pts = Array.from({ length: N }, () => { const a = .65 * g(); return d.map((x) => a * x / l + .12 * g()); }); }
      const C = [0, 1, 2].map((i) => [0, 1, 2].map((j) => pts.reduce((a, p) => a + p[i] * p[j], 0) / N)), { vecs, vals } = jacobi(C);
      if (cross(vecs[0], vecs[1]).reduce((a, x, i) => a + x * vecs[2][i], 0) < 0) vecs[2] = vecs[2].map((x) => -x);   // keep a proper rotation
      pc = { vecs, vals, ...quat(vecs) };               // rows of the rotation are the principal axes
    };
    const turn = (p) => state.rot === 'pca' ? rodrigues(p, pc.ax, pc.ang * state.t / 100) : rodrigues(p, [1 / Math.sqrt(3), 1 / Math.sqrt(3), 1 / Math.sqrt(3)], (Math.PI / 3) * state.t / 100);
    const proj = (p) => {
      const cy = Math.cos(state.yaw), sy = Math.sin(state.yaw), cp = Math.cos(state.pitch), sp = Math.sin(state.pitch), x1 = p[0] * cy - p[1] * sy, y1 = p[0] * sy + p[1] * cy;
      return [CX + S * x1, CY - S * (p[2] * cp + y1 * sp), y1 * cp - p[2] * sp];
    };
    const st = (P) => { let err = 0; const en = [0, 0, 0], tot = P.reduce((a, p) => a + p[0] ** 2 + p[1] ** 2 + p[2] ** 2, 0), per = P.map((p) => { const m = Math.max(...p.map(Math.abs)), step = 2 * m / 3, e = Math.hypot(...p.map((x) => x - (-m + Math.round((x + m) / step) * step))) / Math.hypot(...p); err += e / N; return e; });
      P.forEach((p) => p.forEach((x, i) => { en[i] += x * x / tot; })); return { err, en, per }; };
    const draw = () => {
      const P = pts.map(turn), now = st(P), before = st(pts), kids = [];
      const line = (a, b, cls) => { const [x1, y1] = proj(a), [x2, y2] = proj(b); return mk('line', { x1, y1, x2, y2, class: cls }); };
      [[1.5, 0, 0, 'w₁'], [0, 1.5, 0, 'w₂'], [0, 0, 1.5, 'w₃']].forEach(([x, y, z, t]) => {
        kids.push(line([-x, -y, -z], [x, y, z], 'ax'));
        const [px, py] = proj([x * 1.07, y * 1.07, z * 1.07]), tx = mk('text', { x: px, y: py, class: 'lbl', 'text-anchor': 'middle' }); tx.textContent = t; kids.push(tx);
      });
      if (state.rot === 'pca') pc.vecs.forEach((v, k) => { const L = 1.6 * Math.sqrt(pc.vals[k]) + .25, a = turn(v.map((x) => -x * L)), b = turn(v.map((x) => x * L)); kids.push(line(a, b, 'pax')); });
      P.map((p, i) => ({ p, i, d: proj(p)[2] })).sort((a, b) => b.d - a.d).forEach(({ p, i }) => {
        const [x, y] = proj(p), t = Math.min(1, now.per[i] / .45);
        kids.push(mk('circle', { cx: x, cy: y, r: 3.6, style: `fill:color-mix(in srgb, var(--sx-red) ${t * 100}%, var(--sx-blue))` }));
      });
      svg.replaceChildren(...kids);
      const f = (a) => a.map((x) => (x * 100).toFixed(0) + '%').join(', ');
      note.textContent = `${state.t}% of the ${state.rot === 'pca' ? 'PCA rotation' : 'flattening rotation'}. Average 2-bit rounding error: ${(now.err * 100).toFixed(0)}% (${(before.err * 100).toFixed(0)}% before rotating). Energy in the three coordinates: ${f(now.en)}.`;
    };
    const rebuild = () => { gen(); draw(); };
    fig.querySelectorAll('[data-ds]').forEach((b) => b.addEventListener('click', () => { state.ds = b.dataset.ds; fig.querySelectorAll('[data-ds]').forEach((x) => x.setAttribute('aria-pressed', String(x === b))); rebuild(); }));
    fig.querySelectorAll('[data-rot]').forEach((b) => b.addEventListener('click', () => { state.rot = b.dataset.rot; fig.querySelectorAll('[data-rot]').forEach((x) => x.setAttribute('aria-pressed', String(x === b))); draw(); }));
    $(fig, '[data-k=t]').addEventListener('input', (e) => { state.t = +e.target.value; $(fig, '[data-o=t]').textContent = state.t + '%'; draw(); });
    $(fig, '[data-act=new]').addEventListener('click', () => { seed = (seed * 48271) % 2147483647; rebuild(); });
    let drag = null;
    svg.addEventListener('pointerdown', (e) => { drag = { x: e.clientX, y: e.clientY, yaw: state.yaw, pitch: state.pitch }; });
    window.addEventListener('pointermove', (e) => { if (!drag) return; state.yaw = drag.yaw + (e.clientX - drag.x) * 0.01; state.pitch = Math.max(0.05, Math.min(1.4, drag.pitch + (e.clientY - drag.y) * 0.01)); draw(); });
    window.addEventListener('pointerup', () => { drag = null; });
    spin(fig, state, draw, () => !!drag);
    $(fig, '[data-o=t]').textContent = '0%'; rebuild();
  };

  // 7. Rotation spreads outliers: bars and histograms before and after
  const spreadFig = (fig) => {
    const rng = (seed) => () => (seed = (seed * 16807) % 2147483647) / 2147483647;
    const state = { n: 64, k: 1, s: 20, zoom: false }; let seed = 7;
    const svg = $(fig, 'svg'), note = $(fig, '.sx-note'), NS = 'http://www.w3.org/2000/svg';
    const mk = (tag, attrs) => { const e = document.createElementNS(NS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); return e; };
    const fwht = (x) => { const y = x.slice(), n = y.length; for (let h = 1; h < n; h *= 2) for (let i = 0; i < n; i += 2 * h) for (let j = i; j < i + h; j++) { const a = y[j], b = y[j + h]; y[j] = a + b; y[j + h] = a - b; } return y.map((v) => v / Math.sqrt(n)); };
    const stats = (x) => {                                   // crest factor and the error of plain 16 evenly spaced levels
      const m = Math.max(...x.map(Math.abs)), e = x.reduce((a, v) => a + v * v, 0), step = 2 * m / 15;
      const err = x.reduce((a, v) => a + (-m + Math.round((v + m) / step) * step - v) ** 2, 0);
      return { crest: m / Math.sqrt(e / x.length), err: Math.sqrt(err / e) };
    };
    const bars = (x, R, y0, cls) => {
      const kids = [mk('line', { x1: 20, x2: 620, y1: y0 + 35, y2: y0 + 35, class: 'axis' })], bw = 600 / x.length;
      x.forEach((v, i) => { const h = Math.abs(v) / R * 33; kids.push(mk('rect', { x: 20 + i * bw + (bw > 3 ? .5 : 0), y: v >= 0 ? y0 + 35 - h : y0 + 35, width: Math.max(bw - (bw > 3 ? 1 : 0), .6), height: Math.max(h, .5), class: cls })); });
      return kids;
    };
    const hist = (x, R, y0, cls) => {
      const B = 41, c = new Array(B).fill(0), base = y0 + 40; x.forEach((v) => { c[Math.max(0, Math.min(B - 1, Math.floor((v + R) / (2 * R) * B)))]++; });
      const top = Math.max(1, ...c), bw = 600 / B, kids = [mk('line', { x1: 20, x2: 620, y1: base, y2: base, class: 'axis' })];
      c.forEach((v, i) => { if (v) kids.push(mk('rect', { x: 20 + i * bw + 1, y: base - v / top * 36, width: bw - 2, height: v / top * 36, class: cls })); });
      [[20, 'start', -R], [320, 'middle', 0], [620, 'end', R]].forEach(([x0, anchor, val]) => { const t = mk('text', { x: x0, y: base + 13, 'text-anchor': anchor, class: 'lbl' }); t.textContent = (val > 0 ? '+' : '') + (Math.abs(val) < 10 ? val.toFixed(1) : Math.round(val)); kids.push(t); });
      return kids;
    };
    const draw = () => {
      const r = rng(seed), g = () => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r());
      const x = Array.from({ length: state.n }, g), k = Math.min(state.k, state.n);
      for (let j = 0; j < k; j++) x[Math.floor(r() * state.n)] = (r() < .5 ? -1 : 1) * state.s;
      const y = fwht(x), mx = Math.max(...x.map(Math.abs)), my = Math.max(...y.map(Math.abs)), R = Math.max(mx, my);
      const Rx = state.zoom ? mx : R, Ry = state.zoom ? my : R;
      const label = (txt, yy) => { const t = mk('text', { x: 20, y: yy, class: 'lbl' }); t.textContent = txt; return t; };
      svg.replaceChildren(label('before rotation', 10), ...bars(x, R, 12, 'bar before'), ...hist(x, Rx, 50, 'bar before'),
        label('after rotation', 175), ...bars(y, R, 177, 'bar after'), ...hist(y, Ry, 215, 'bar after'));
      const a = stats(x), b = stats(y);
      note.textContent = `${state.n} weights, ${k} outlier${k === 1 ? '' : 's'}. Largest value over typical value: ${a.crest.toFixed(1)}× before, ${b.crest.toFixed(1)}× after. `
        + `Rounding to 16 evenly spaced levels sized to the largest value gives an error of ${(a.err * 100).toFixed(0)}% before and ${(b.err * 100).toFixed(0)}% after.`;
    };
    fig.querySelectorAll('input').forEach((i) => i.addEventListener('input', () => {
      state[i.dataset.k] = i.dataset.k === 'n' ? 2 ** +i.value : +i.value;
      $(fig, `[data-o=${i.dataset.k}]`).textContent = state[i.dataset.k]; draw();
    }));
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.act === 'new') seed = (seed * 48271) % 2147483647;
      else { state.zoom = !state.zoom; b.setAttribute('aria-pressed', String(state.zoom)); }
      draw();
    }));
    draw();
  };

  // 6b. PCA versus the Hadamard rotation on the same groups of 8 weights
  const pcaFig = (fig) => {
    const M = 24, D = 8, CLIP = 4, state = { ds: 'spiky' }; let seed = 3;
    const rng = (sd) => () => (sd = (sd * 16807) % 2147483647) / 2147483647;
    const svg = $(fig, 'svg'), note = $(fig, '.sx-note'), NS = 'http://www.w3.org/2000/svg';
    const mk = (tag, attrs) => { const e = document.createElementNS(NS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); return e; };
    const data = () => {
      const r = rng(seed), g = () => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r());
      if (state.ds === 'spiky') return Array.from({ length: M }, () => { const row = Array.from({ length: D }, () => .35 * g()); row[Math.floor(r() * D)] += (r() < .5 ? -1 : 1) * (3.5 + .6 * g()); return row; });
      const u = Array.from({ length: D }, g), l = Math.hypot(...u), uu = u.map((x) => x / l);
      return Array.from({ length: M }, () => { const a = g(); return uu.map((x) => 3.2 * a * x + .45 * g()); });
    };
    const eig = (C) => jacobi(C).vecs;                  // eigenvectors, biggest first, as arrays
    const pca = (X) => {
      const C = Array.from({ length: D }, (_, i) => Array.from({ length: D }, (_, j) => X.reduce((a, r) => a + r[i] * r[j], 0) / M));
      const cols = eig(C);                               // cols[k][i] = component i of eigenvector k
      return X.map((r) => cols.map((v) => r.reduce((a, x, i) => a + x * v[i], 0)));
    };
    const fwht = (x) => { const y = x.slice(); for (let h = 1; h < D; h *= 2) for (let i = 0; i < D; i += 2 * h) for (let j = i; j < i + h; j++) { const a = y[j], b = y[j + h]; y[j] = a + b; y[j + h] = a - b; } return y.map((v) => v / Math.sqrt(D)); };
    const stats = (Y) => {
      const tot = Y.reduce((a, r) => a + r.reduce((b, x) => b + x * x, 0), 0), cols = Array.from({ length: D }, (_, c) => Y.reduce((a, r) => a + r[c] * r[c], 0) / tot);
      let spike = 0, err = 0;
      Y.forEach((r) => { const len = Math.hypot(...r), m = Math.max(...r.map(Math.abs)), st = 2 * m / 3; spike += m / len / M; err += Math.hypot(...r.map((x) => x - (-m + Math.round((x + m) / st) * st))) / len / M; });
      return { cols, spike, err };
    };
    const draw = () => {
      const X = data(), views = [['As given', X], ['After PCA', pca(X)], ['After a flattening rotation', X.map(fwht)]], kids = [], W = 260, CW = W / D, RH = 5, res = [];
      views.forEach(([title, Y], v) => {
        const x0 = 20 + v * 295, st = stats(Y); res.push(st);
        const t = mk('text', { x: x0, y: 14, class: 'lbl' }); t.textContent = title; kids.push(t);
        Y.forEach((r, i) => r.forEach((val, c) => { const k = Math.max(-1, Math.min(1, val / CLIP)); kids.push(mk('rect', { x: x0 + c * CW, y: 22 + i * RH, width: CW - 1, height: RH, style: `fill:color-mix(in srgb, var(${k >= 0 ? '--sx-red' : '--sx-blue'}) ${Math.abs(k) * 100}%, var(--bg))` })); }));
        st.cols.forEach((e, c) => { const h = Math.max(1, e * 56); kids.push(mk('rect', { x: x0 + c * CW, y: 22 + M * RH + 10 + 56 - h, width: CW - 1, height: h, class: 'pcb' })); });
        const by = 22 + M * RH + 10 + 56 + 14;
        [`busiest coordinate: ${(Math.max(...st.cols) * 100).toFixed(0)}% of the energy`, `largest weight: ${(st.spike * 100).toFixed(0)}% of its group's length`, `2-bit rounding error: ${(st.err * 100).toFixed(0)}%`].forEach((txt, i) => { const tx = mk('text', { x: x0, y: by + i * 14, class: 'lbl' }); tx.textContent = txt; kids.push(tx); });
      });
      svg.replaceChildren(...kids);
      note.textContent = state.ds === 'spiky'
        ? `Every group has one big weight in a random place, so no single direction stands out and PCA has little to find: the error only moves from ${(res[0].err * 100).toFixed(0)}% to ${(res[1].err * 100).toFixed(0)}%. A flattening rotation spreads each spike over all 8 coordinates (largest weight ${(res[0].spike * 100).toFixed(0)}% → ${(res[2].spike * 100).toFixed(0)}% of the length) and the error falls to ${(res[2].err * 100).toFixed(0)}%.`
        : `The weights share one direction. PCA finds it and moves nearly all the energy into the first coordinate (${(Math.max(...res[1].cols) * 100).toFixed(0)}%), which is ideal for dropping the other seven, but each group becomes one big weight plus noise: the error rises from ${(res[0].err * 100).toFixed(0)}% to ${(res[1].err * 100).toFixed(0)}%. A flattening rotation does not need to know the direction and spreads everything out.`;
    };
    fig.querySelectorAll('[data-ds]').forEach((b) => b.addEventListener('click', () => { state.ds = b.dataset.ds; fig.querySelectorAll('[data-ds]').forEach((x) => x.setAttribute('aria-pressed', String(x === b))); draw(); }));
    $(fig, '[data-act=new]').addEventListener('click', () => { seed = (seed * 48271) % 2147483647; draw(); });
    draw();
  };

  // 8. The fast Walsh–Hadamard transform, round by round
  const fwhtFig = (fig) => {
    const x0 = [3, 1, -2, 4, 0, 2, 1, -1], N = x0.length, ROUNDS = Math.log2(N);
    const stages = [x0.slice()];
    for (let r = 0, h = 1; r < ROUNDS; r++, h *= 2) {
      const p = stages[r], v = p.slice();
      for (let i = 0; i < N; i += 2 * h) for (let j = i; j < i + h; j++) { v[j] = p[j] + p[j + h]; v[j + h] = p[j] - p[j + h]; }
      stages.push(v);
    }
    const box = $(fig, '.sx-fwht'), note = $(fig, '.sx-note');
    let k = 0;
    const draw = () => {
      const t = el('table');
      const hr = el('tr'); hr.append(el('th', null, 'input'));
      for (let r = 1; r <= ROUNDS; r++) hr.append(el('th', null, `round ${r}`));
      hr.append(mel('th', null, `\\(\\div\\sqrt{${N}}\\)`)); t.append(hr);
      const h = k ? 1 << (k - 1) : 0;
      for (let i = 0; i < N; i++) {
        const tr = el('tr'); tr.append(el('td', 'num', String(x0[i])));
        for (let r = 1; r <= ROUNDS; r++) {
          const td = el('td', 'num', r <= k ? String(stages[r][i]) : '');
          if (r === k) td.classList.add(Math.floor(i / h) % 2 ? 'b' : 'a');
          tr.append(td);
        }
        tr.append(el('td', 'num', k === ROUNDS ? (stages[ROUNDS][i] / Math.sqrt(N)).toFixed(2) : ''));
        t.append(tr);
      }
      box.replaceChildren(t);
      setMath(note, k === 0
        ? `${N} activations. Multiplying by the 8×8 matrix directly would take \\(8\\times 8 = 64\\) multiply-adds.`
        : k < ROUNDS
          ? `Round ${k}: pairs ${h} apart are combined. Blue rows got a + b, gold rows a − b.`
          : `Done after ${ROUNDS} rounds of ${N} additions or subtractions (\\(${ROUNDS}\\times ${N} = ${ROUNDS * N}\\) in all, no multiplications), then one scaling. For a group of 64 it's \\(6\\times 64 = 384\\) operations instead of \\(64^2 = 4{,}096\\).`);
    };
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.act === 'next') k = k >= ROUNDS ? 0 : k + 1;
      if (b.dataset.act === 'reset') k = 0;
      draw();
    }));
    draw();
  };

  // 0. Plain quantization: fewer bits, coarser rounding, what an outlier does to it, and the bits themselves
  const quantizeFig = (fig) => {
    const N = 40, SIGMA = 0.05, RANGE = 0.5, state = { bits: 3, outlier: false, sel: 3 }; let seed = 5;
    const rng = (sd) => () => (sd = (sd * 16807) % 2147483647) / 2147483647;
    const svg = $(fig, 'svg'), note = $(fig, '.sx-note'), panel = $(fig, '.sx-bits'), NS = 'http://www.w3.org/2000/svg';
    const mk = (tag, attrs) => { const e = document.createElementNS(NS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); return e; };
    const f32 = (x) => { const d = new DataView(new ArrayBuffer(4)); d.setFloat32(0, x); return d.getUint32(0).toString(2).padStart(32, '0'); };
    const boxes = (str, cls) => { const w = el('div', 'sx-bitrow'); [...str].forEach((c, i) => { const b = el('span', 'b ' + (cls instanceof Function ? cls(i) : cls), c); w.append(b); }); return w; };
    const draw = () => {
      const r = rng(seed), g = () => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r());
      const w = Array.from({ length: N }, () => g() * SIGMA); if (state.outlier) w[Math.floor(r() * N)] = 9 * SIGMA * (r() < .5 ? -1 : 1);
      const m = Math.max(...w.map(Math.abs)), n = 2 ** state.bits, step = 2 * m / (n - 1);
      const idxOf = (x) => Math.round((x + m) / step), levels = Array.from({ length: n }, (_, k) => -m + k * step), nearest = (x) => -m + idxOf(x) * step;
      const X = (x) => 20 + ((x + RANGE) / (2 * RANGE)) * 600, big = w.findIndex((x) => Math.abs(x) === m);
      const kids = [mk('line', { x1: 20, x2: 620, y1: 128, y2: 128, class: 'axis' })];
      levels.forEach((l) => kids.push(mk('line', { x1: X(l), x2: X(l), y1: 120, y2: 136, class: 'level' })));
      w.forEach((x, i) => {
        const y = 34 + (i % 6) * 13;
        kids.push(mk('line', { x1: X(x), x2: X(nearest(x)), y1: y, y2: 120, class: 'snap' }));
        kids.push(mk('circle', { cx: X(x), cy: y, r: 4, class: (state.outlier && i === big ? 'dot out' : 'dot') + (i === state.sel ? ' sel' : ''), 'data-i': i }));
      });
      // level numbers under the ticks: decimal, and binary when it fits (staggered over two rows)
      const gap = X(step) - X(0), selK = idxOf(w[state.sel]);
      let stride = 1; while (gap * stride < 15 && stride < n) stride *= 2;
      const showBin = gap * stride * 2 >= state.bits * 6.2 + 4;
      const text = (x, y, str, cls) => { const t = mk('text', { x, y, 'text-anchor': 'middle', class: cls }); t.textContent = str; return t; };
      levels.forEach((l, k) => {
        if (k !== selK && (k % stride || Math.abs(X(l) - X(levels[selK])) < state.bits * 6.2 + 4)) return;
        const cls = k === selK ? ' selk' : '';
        kids.push(text(X(l), 150, String(k), 'lv' + cls));
        if (showBin || k === selK) kids.push(text(X(l), 162 + (showBin && (k / stride) % 2 ? 11 : 0), k.toString(2).padStart(state.bits, '0'), 'bn' + cls));
      });
      // value ruler above the dots: the actual weights
      kids.push(mk('line', { x1: 20, x2: 620, y1: 18, y2: 18, class: 'axis' }));
      for (let t = -10; t <= 10; t++) {
        const v = t * 0.05, major = t % 5 === 0;
        kids.push(mk('line', { x1: X(v), x2: X(v), y1: 18, y2: major ? 25 : 22, class: 'axis' }));
        if (major) kids.push(text(X(v), 11, String(Number(v.toFixed(2))), 'lbl'));
      }
      svg.replaceChildren(...kids);
      const e2 = w.reduce((a, x) => a + (x - nearest(x)) ** 2, 0), p2 = w.reduce((a, x) => a + x * x, 0);
      setMath(note, `${state.bits} bit${state.bits > 1 ? 's' : ''} give \\(2^{${state.bits}} = ${n}\\) levels, ${Number((32 / state.bits).toFixed(1))}× smaller than 32-bit floats. Rounding error: ${(Math.sqrt(e2 / p2) * 100).toFixed(0)}% of the weights' own size. Gap between neighbouring levels: ${step.toFixed(3)}.`);
      const x = w[state.sel], k = idxOf(x), bits = f32(x);
      panel.replaceChildren(
        el('p', null, `Selected weight: ${x.toFixed(4)}. As a 32-bit float it is these bits (sign, exponent, mantissa):`),
        boxes(bits, (i) => (i === 0 ? 's' : i < 9 ? 'e' : 'm')),
        el('p', null, `Quantized, it is only the number of its nearest level, counting from the left (0 to ${n - 1}). That is ${k}, written in ${state.bits} bit${state.bits > 1 ? 's' : ''}:`),
        boxes(k.toString(2).padStart(state.bits, '0'), 'i'),
        mel('p', null, `To read it back: \\(\\mathrm{level} = -m + \\mathrm{index}\\times\\mathrm{gap} = -${m.toFixed(4)} + ${k}\\times ${step.toFixed(4)} = ${nearest(x).toFixed(4)}\\). The weight was ${x.toFixed(4)}, so the error is ${Math.abs(x - nearest(x)).toFixed(4)}.`),
        mel('p', null, `All ${N} weights as 32-bit floats: \\(${N}\\times 32 = ${N * 32}\\) bits. Quantized: \\(${N}\\times ${state.bits} = ${N * state.bits}\\) bits for the indices, plus \\(m = ${m.toFixed(4)}\\) stored once as a 32-bit float, so \\(${N * state.bits} + 32 = ${N * state.bits + 32}\\) bits in all.`));
    };
    svg.addEventListener('click', (e) => { const i = e.target.dataset && e.target.dataset.i; if (i != null) { state.sel = +i; draw(); } });
    $(fig, '[data-k=bits]').addEventListener('input', (e) => { state.bits = +e.target.value; $(fig, '[data-o=bits]').textContent = state.bits; draw(); });
    $(fig, '[data-k=outlier]').addEventListener('click', (e) => { state.outlier = !state.outlier; e.currentTarget.setAttribute('aria-pressed', String(state.outlier)); draw(); });
    $(fig, '[data-act=new]').addEventListener('click', () => { seed = (seed * 48271) % 2147483647; draw(); });
    $(fig, '[data-o=bits]').textContent = state.bits; draw();
  };

  // 9. Why weights are grouped: a toy matrix, per-group scales, rounding error and storage per row
  const groupingFig = (fig) => {
    const R = 12, C = 64, CW = 10, RH = 11, BARH = 46, SIZES = [64, 32, 16, 8, 4], NS = 'http://www.w3.org/2000/svg';
    const state = { g: 3, out: true, nout: 6, size: 8, sel: 0 }; let seed = 7;
    const rng = () => (seed = (seed * 16807) % 2147483647) / 2147483647;
    const gauss = () => Math.sqrt(-2 * Math.log(rng() + 1e-12)) * Math.cos(2 * Math.PI * rng());
    let base = [], outl = [], outList = [];
    const gen = () => {
      base = Array.from({ length: R }, () => { const s = Math.exp(.35 * gauss()); return Array.from({ length: C }, () => gauss() * s); });
      const rows = Array.from({ length: R }, (_, i) => i);     // outliers go in different rows, so each one is in a group of its own at any group size
      for (let i = R - 1; i > 0; i--) { const j = Math.floor(rng() * (i + 1)); [rows[i], rows[j]] = [rows[j], rows[i]]; }
      outList = rows.map((r0) => ({ r: r0, c: Math.floor(rng() * C), sign: rng() < .5 ? -1 : 1, u: rng() }));
      applyOut();
      state.sel = Math.floor(rng() * R);
    };
    const applyOut = () => { outl = Array.from({ length: R }, () => new Array(C).fill(0)); outList.slice(0, state.nout).forEach(({ r, c, sign, u }) => { outl[r][c] = sign * state.size * (.75 + .5 * u); }); state.out = state.nout > 0; };
    const val = (r, c) => (state.out && outl[r][c] ? outl[r][c] : base[r][c]);
    const mk = (tag, attrs, style) => { const e = document.createElementNS(NS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); if (style) e.style.cssText = style; return e; };
    const stats = (gs) => {                              // 16 evenly spaced levels per group, sized to its largest weight
      const err = [], scales = []; let e2 = 0, w2 = 0;
      for (let r = 0; r < R; r++) {
        const er = [], sc = [];
        for (let g = 0; g < C / gs; g++) {
          let mx = 0; for (let c = g * gs; c < (g + 1) * gs; c++) mx = Math.max(mx, Math.abs(val(r, c)));
          const step = 2 * mx / 15; sc.push({ mx, step });
          for (let c = g * gs; c < (g + 1) * gs; c++) { const x = val(r, c), q = step ? Math.round((x + mx) / step) * step - mx : 0; er.push(Math.abs(x - q)); e2 += (x - q) ** 2; w2 += x * x; }
        }
        err.push(er); scales.push(sc);
      }
      return { err, scales, rel: Math.sqrt(e2 / w2), rowBits: C * 4 + (C / gs) * 32 };
    };
    const wsvg = $(fig, '[data-v=w]'), ssvg = $(fig, '[data-v=s]'), esvg = $(fig, '[data-v=e]'), info = $(fig, '[data-v=info]'), store = $(fig, '[data-v=store]'), tab = $(fig, '.sx-gtab');
    const MAXBITS = C * 4 + 16 * 32;
    let cur = null, curGs = 64;
    const draw = () => {
      const gs = SIZES[state.g], ng = C / gs, st = stats(gs); cur = st; curGs = gs;
      $(fig, '[data-o=g]').textContent = `${ng} of ${gs}`;
      let gm = 0; for (let r = 0; r < R; r++) for (let c = 0; c < C; c++) gm = Math.max(gm, Math.abs(val(r, c)));
      const common = { R, C, CW, RH, gs };
      const vb = `0 0 ${C * CW} ${R * RH}`;
      wsvg.setAttribute('viewBox', vb); wsvg.replaceChildren(...matrixCells({ ...common, color: (r, c) => divColor(val(r, c), 3), outline: (r, c) => state.out && outl[r][c], label: (r, g) => st.scales[r][g].mx.toFixed(1) }));
      ssvg.setAttribute('viewBox', vb); ssvg.replaceChildren(...matrixGroups({ ...common, color: (r, g) => goldColor(st.scales[r][g].mx, gm), outline: (r, g) => state.out && outl[r].slice(g * gs, (g + 1) * gs).some((v) => v) }));
      esvg.setAttribute('viewBox', vb); esvg.replaceChildren(...matrixCells({ ...common, color: (r, c) => redColor(st.err[r][c], .5), outline: (r, c) => state.out && outl[r][c] }));
      const groups = []; for (let r = 0; r < R; r++) for (let g = 0; g < ng; g++) { const e = st.err[r].slice(g * gs, (g + 1) * gs); groups.push({ outlier: state.out && outl[r].slice(g * gs, (g + 1) * gs).some((v) => v), loss: e.reduce((a, x) => a + x, 0) / gs }); }
      publishGrouping({ gs, ng, outliersOn: state.out, groups, values: Array.from({ length: R }, (_, r) => Array.from({ length: C }, (_, c) => val(r, c))) });
      const idx = C * 4, scl = ng * 32;
      $(fig, '.sx-gbar .idx').style.width = `${idx / MAXBITS * 100}%`; $(fig, '.sx-gbar .scl').style.width = `${scl / MAXBITS * 100}%`;
      setMath(store, `Storage per row: \\(${C}\\times 4 = ${idx}\\) bits for the indices, plus \\(${ng}\\times 32 = ${scl}\\) bits for the scale${ng > 1 ? 's' : ''}, \\(${st.rowBits}\\) bits in all (${st.rowBits / 8} bytes). Relative error: ${(st.rel * 100).toFixed(1)}%.`);
      const rows = [['groups per row', 'error', 'bits per row']];
      SIZES.forEach((sz, i) => { const s2 = stats(sz); rows.push([`${C / sz} of ${sz}`, `${(s2.rel * 100).toFixed(1)}%`, String(s2.rowBits), i]); });
      tab.replaceChildren(...rows.map((r, i) => { const tr = el('tr'); r.slice(0, 3).forEach((t, j) => tr.append(el(i ? 'td' : 'th', j ? 'num' : null, t))); if (i) { tr.dataset.i = r[3]; if (r[3] === state.g) tr.className = 'on'; } return tr; }));
    };
    const pick = (e) => {
      const e0 = e.currentTarget.getBoundingClientRect();
      const x = (e.clientX - e0.left) / e0.width * C * CW, y = (e.clientY - e0.top) / e0.width * C * CW;
      return { c: Math.floor(x / CW), r: Math.floor(y / RH) };
    };
    [wsvg, ssvg, esvg].forEach((svg) => {
      svg.addEventListener('mousemove', (e) => {
        const p = pick(e); if (p.r < 0 || p.r >= R || p.c < 0 || p.c >= C) return;
        const g = Math.floor(p.c / curGs), s = cur.scales[p.r][g];
        info.textContent = `Row ${p.r + 1}, group ${g + 1} of ${C / curGs}: weight ${val(p.r, p.c).toFixed(2)}, largest in group ${s.mx.toFixed(2)}, gap between levels ${s.step.toFixed(2)}, error ${cur.err[p.r][p.c].toFixed(2)}.`;
      });
    });
    tab.addEventListener('click', (e) => { const tr = e.target.closest('tr[data-i]'); if (tr) { state.g = +tr.dataset.i; $(fig, '[data-k=g]').value = state.g; draw(); } });
    $(fig, '[data-k=g]').addEventListener('input', (e) => { state.g = +e.target.value; draw(); });
    $(fig, '[data-k=nout]').addEventListener('input', (e) => { state.nout = +e.target.value; $(fig, '[data-o=nout]').textContent = state.nout; applyOut(); draw(); });
    $(fig, '[data-k=osize]').addEventListener('input', (e) => { state.size = +e.target.value; $(fig, '[data-o=osize]').textContent = state.size + '×'; applyOut(); draw(); });
    $(fig, '[data-act=new]').addEventListener('click', () => { seed = (seed * 48271) % 2147483647; gen(); draw(); });
    info.textContent = 'Hover a cell to see its group.'; $(fig, '[data-o=nout]').textContent = state.nout; $(fig, '[data-o=osize]').textContent = state.size + '×'; gen(); draw();
  };

  // 9a. The groups of the grouping figure, split into those that contain an outlier and the rest: how much each loses to rounding
  const outlierLossFig = (fig) => {
    const svg = $(fig, 'svg'), note = $(fig, '.sx-note');
    const mean = (a) => a.reduce((x, y) => x + y, 0) / (a.length || 1);
    const txt = (x, y, str, anchor, cls) => { const t = sv('text', { x, y, 'text-anchor': anchor, class: cls || 'lbl' }); t.textContent = str; return t; };
    const render = (d) => {
      const withO = d.groups.filter((g) => g.outlier), without = d.groups.filter((g) => !g.outlier), all = d.groups.map((g) => g.loss);
      const top = Math.max(...all) * 1.08, x0 = 170, x1 = 620, X = (v) => x0 + (x1 - x0) * v / top, kids = [];
      const rows = [['With an outlier', withO, 'var(--sx-gold)', 52], ['Without', without, 'var(--sx-blue)', 118]];
      kids.push(sv('line', { x1: x0, x2: x1, y1: 156, y2: 156, class: 'ax' }));
      const step = top > 1 ? .5 : top > .5 ? .25 : .1;
      for (let v = 0; v <= top; v += step) { kids.push(sv('line', { x1: X(v), x2: X(v), y1: 156, y2: 161, class: 'ax' })); kids.push(txt(X(v), 174, v.toFixed(2).replace(/0$/, ''), 'middle')); }
      kids.push(txt((x0 + x1) / 2, 192, "average rounding error of the group's weights", 'middle'));
      rows.forEach(([name, list, color, y]) => {
        kids.push(txt(10, y - 4, name, 'start', 'lbl rowname'), txt(10, y + 12, `${list.length} group${list.length === 1 ? '' : 's'}`, 'start'));
        list.forEach((g, i) => kids.push(sv('circle', { cx: X(g.loss), cy: y + (((i * 37) % 17) - 8) * 2, r: 3.6, class: 'odot' }, `fill:${color}`)));
        if (list.length) { const m = mean(list.map((g) => g.loss)); kids.push(sv('line', { x1: X(m), x2: X(m), y1: y - 38, y2: y + 38, class: 'mean' })); kids.push(txt(X(m), y - 42, 'mean ' + m.toFixed(2), 'middle', 'lbl meant')); }
      });
      svg.replaceChildren(...kids);
      note.textContent = !d.outliersOn || !withO.length
        ? 'No group has an outlier right now. Raise "Groups with outliers" in the figure above (or press "New matrix") to see them.'
        : `Groups of ${d.gs}: the ${withO.length} with an outlier lose ${mean(withO.map((g) => g.loss)).toFixed(2)} on average, the other ${without.length} lose ${mean(without.map((g) => g.loss)).toFixed(2)}. That is ${(mean(withO.map((g) => g.loss)) / mean(without.map((g) => g.loss))).toFixed(1)} times as much, for weights that are almost all ordinary.`;
    };
    groupingShare.subs.push(render); if (groupingShare.data) render(groupingShare.data);
  };

  // 9b. Histograms of SmolLM2's real weights before and after rotating each group (data from convert/weight_hist.py)
  const histFig = (fig) => {
    const D = window.WEIGHT_HIST, svg = $(fig, 'svg'), note = $(fig, '.sx-note');
    if (!D) { note.textContent = 'The weight statistics did not load.'; return; }
    const SIZES = [8, 16, 32, 64], LABEL = { all: 'all layers', attention: 'attention layers', mlp: 'feed-forward layers', embedding: 'embedding' };
    const st = { cls: 'all', g: 3, log: false, norm: true };
    const txt = (x, y, str, anchor, cls) => { const t = sv('text', { x, y, 'text-anchor': anchor, class: cls || 'lbl' }); t.textContent = str; return t; };
    const draw = () => {
      const G = SIZES[st.g], e = D.data[`${st.cls}/${G}`], d = st.norm ? e : { ...e, before: e.rawbefore, after: e.rawafter, kurt: e.rawkurt, beyond3: e.rawbeyond3, max: e.rawmax }, NB = D.bins.n, bw = D.bins.width, xm = -D.bins.lo, n = d.n;
      $(fig, '[data-o=g]').textContent = G;
      fig.querySelectorAll('[data-cls]').forEach((b) => b.setAttribute('aria-pressed', String(b.dataset.cls === st.cls)));
      $(fig, '[data-k=log]').setAttribute('aria-pressed', String(st.log)); $(fig, '[data-k=norm]').setAttribute('aria-pressed', String(st.norm));
      const dens = (z) => Math.exp(-z * z / 2) / Math.sqrt(2 * Math.PI);
      const top = Math.max(...d.before, ...d.after, n * bw * dens(0)) * 1.08;
      const T = (k) => (st.log ? Math.log10(1 + k) / Math.log10(1 + top) : k / top);
      const kids = [], W = 296, H = 130, Y0 = 36, cw = (W - 16) / NB;
      [['Before rotation', d.before, 'var(--sx-blue)'], ['After rotating each group', d.after, 'var(--sx-gold)']].forEach(([title, c, color], p) => {
        const ox = p * 324, X = (v) => ox + 8 + (W - 16) * (v + xm) / (2 * xm), Y = (k) => Y0 + H - H * T(k);
        kids.push(txt(ox + W / 2, 16, title, 'middle', 'lbl rowname'));
        c.forEach((k, i) => { if (!k) return; const h = Math.max(1, H * T(k)); kids.push(sv('rect', { x: ox + 8 + cw * i + .2, y: Y0 + H - h, width: cw - .4, height: h }, `fill:${color};opacity:.85`)); });
        const pts = []; for (let i = 0; i <= 120; i++) { const z = -xm + 2 * xm * i / 120; pts.push(`${X(z).toFixed(1)},${Y(n * bw * dens(z)).toFixed(1)}`); }
        kids.push(sv('polyline', { points: pts.join(' '), fill: 'none', class: 'mean' }, 'stroke-dasharray:4 3;stroke-width:1.5'));
        kids.push(sv('line', { x1: ox + 8, x2: ox + W - 8, y1: Y0 + H, y2: Y0 + H, class: 'ax' }));
        for (let t = -6; t <= 6; t += 2) { kids.push(sv('line', { x1: X(t), x2: X(t), y1: Y0 + H, y2: Y0 + H + 4, class: 'ax' })); kids.push(txt(X(t), Y0 + H + 16, t ? (t > 0 ? '+' : '−') + Math.abs(t) + 'σ' : '0', 'middle')); }
      });
      svg.replaceChildren(...kids);
      const f = (v) => v.toFixed(v < 10 ? 1 : 0);
      note.textContent = `${D.model}, ${LABEL[st.cls]}, groups of ${G} (${(n / 1e6).toFixed(0)} million weights). Kurtosis (3 for a bell curve): ${d.kurt[0].toFixed(2)} before, ${d.kurt[1].toFixed(2)} after. Beyond 3σ (a bell curve has 0.27%): ${d.beyond3[0]}% before, ${d.beyond3[1]}% after. Largest value: ${f(d.max[0])}σ before, ${f(d.max[1])}σ after. ` + (st.norm ? `A group of ${G} cannot hold a value beyond √${G} = ${Math.sqrt(G).toFixed(1)}σ.` : 'Here only each whole matrix is scaled, so groups keep their own sizes, and values beyond ±6σ are counted in the end bars.');
    };
    fig.querySelectorAll('[data-cls]').forEach((b) => b.addEventListener('click', () => { st.cls = b.dataset.cls; draw(); }));
    $(fig, '[data-k=g]').addEventListener('input', (e) => { st.g = +e.target.value; draw(); });
    $(fig, '[data-k=log]').addEventListener('click', () => { st.log = !st.log; draw(); });
    $(fig, '[data-k=norm]').addEventListener('click', () => { st.norm = !st.norm; draw(); });
    draw();
  };

  // 10. Lloyd's algorithm, step by step: the levels start evenly spaced and move to where the values are
  const lloydFig = (fig) => {
    const svg = $(fig, 'svg'), note = $(fig, '.sx-readout'), N = 4000, st = { bits: 2, step: 0 };
    const r = rng2(11), gs = () => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r());
    const xs = Array.from({ length: N }, gs), BINS = 61, LO = -4, HI = 4, hist = new Array(BINS).fill(0);
    xs.forEach((x) => { const b = Math.floor((x - LO) / (HI - LO) * BINS); if (b >= 0 && b < BINS) hist[b]++; });
    let levels = [], errs = [], timer = 0;
    const mse = (lv) => xs.reduce((a, x) => { let m = 1e9; for (const l of lv) m = Math.min(m, (x - l) ** 2); return a + m; }, 0) / N;
    const init = () => { clearInterval(timer); const L = 2 ** st.bits; levels = Array.from({ length: L }, (_, k) => -3 + k * 6 / (L - 1)); errs = [mse(levels)]; st.step = 0; };
    const step = () => {
      const L = levels.length, sum = new Array(L).fill(0), cnt = new Array(L).fill(0);
      xs.forEach((x) => { let bi = 0, bd = 1e9; levels.forEach((l, k) => { const d = (x - l) ** 2; if (d < bd) { bd = d; bi = k; } }); sum[bi] += x; cnt[bi]++; });
      const next = levels.map((l, k) => (cnt[k] ? sum[k] / cnt[k] : l)), moved = Math.max(...next.map((l, k) => Math.abs(l - levels[k])));
      levels = next; errs.push(mse(levels)); st.step++; return moved;
    };
    const X = (v) => 20 + (v - LO) / (HI - LO) * 600, top = Math.max(...hist), word = (x, y, t, a, c) => { const e = sv('text', { x, y, 'text-anchor': a, class: c || 'lbl' }); e.textContent = t; return e; };
    const draw = () => {
      const kids = [], bw = 600 / BINS;
      hist.forEach((c, i) => { const h = c / top * 110; kids.push(sv('rect', { x: 20 + i * bw + 1, y: 150 - h, width: bw - 2, height: h, class: 'bar' })); });
      kids.push(sv('line', { x1: 20, x2: 620, y1: 150, y2: 150, class: 'ax' }));
      for (let v = -3; v <= 3; v++) { kids.push(sv('line', { x1: X(v), x2: X(v), y1: 150, y2: 155, class: 'ax' }), word(X(v), 170, v, 'middle')); }
      kids.push(word(320, 190, 'value (in units of the typical size of a weight)', 'middle'));
      for (let k = 0; k < levels.length - 1; k++) { const cut = (levels[k] + levels[k + 1]) / 2; kids.push(sv('line', { x1: X(cut), x2: X(cut), y1: 30, y2: 150, class: 'cut' })); }
      levels.forEach((l) => kids.push(sv('line', { x1: X(l), x2: X(l), y1: 22, y2: 150, class: 'lv' })));
      if (levels.length <= 4) levels.forEach((l) => kids.push(word(X(l), 16, l.toFixed(2), 'middle', 'lbl lvt')));
      svg.replaceChildren(...kids);
      const e0 = errs[0], e1 = errs[errs.length - 1], pc = (v) => (v * 100).toFixed(1) + '%';
      note.textContent = st.step === 0
        ? `${levels.length} levels, evenly spaced. Average squared error: ${pc(e0)} of the values' variance. Press "next step".`
        : `Step ${st.step}: error ${pc(e1)}, down from ${pc(e0)} with evenly spaced levels (${((1 - e1 / e0) * 100).toFixed(0)}% less). The levels have crowded toward the middle, where most values are.`;
    };
    $(fig, '[data-k=bits]').addEventListener('input', (e) => { st.bits = +e.target.value; $(fig, '[data-o=bits]').textContent = st.bits; init(); draw(); });
    $(fig, '[data-act=next]').addEventListener('click', () => { step(); draw(); });
    $(fig, '[data-act=reset]').addEventListener('click', () => { init(); draw(); });
    $(fig, '[data-act=run]').addEventListener('click', () => { clearInterval(timer); timer = setInterval(() => { const moved = step(); draw(); if (moved < 1e-3 || st.step >= 60) clearInterval(timer); }, 350); });
    $(fig, '[data-o=bits]').textContent = st.bits; init(); draw();
  };

  // 11. What a model costs to store: bits per weight and group size against the iPod's RAM
  const sizeCalcFig = (fig) => {
    const svg = $(fig, 'svg'), note = $(fig, '.sx-readout'), M = 134.5e6, G = [16, 32, 64, 128, 256], RAM = 256;
    const draw = () => {
      const bits = +$(fig, '[data-k=bits]').value, g = G[+$(fig, '[data-k=g]').value], bpw = bits + 32 / g, mb = M * bpw / 8 / 1e6, f32 = M * 4 / 1e6;
      $(fig, '[data-o=bits]').textContent = bits; $(fig, '[data-o=g]').textContent = g;
      const X = (v) => 190 + v / 600 * 430, kids = [], word = (x, y, t, a, c) => { const e = sv('text', { x, y, 'text-anchor': a, class: c || 'lbl' }); t && (e.textContent = t); return e; };
      const wMB = M * bits / 8 / 1e6, sMB = M * 32 / g / 8 / 1e6, GOLD = 'var(--sx-gold)', BLUE = 'var(--sx-blue)';
      [['32-bit floats', [[f32, 'var(--muted)']], 30], ['this setting', [[wMB, BLUE], [sMB, GOLD]], 80]].forEach(([name, parts, y]) => {
        let acc = 0; const total = parts.reduce((t, p) => t + p[0], 0);
        kids.push(word(180, y + 15, name, 'end', 'lbl rowname'));
        parts.forEach(([v, color]) => { const x0 = X(acc), x1 = X(acc + v); kids.push(sv('rect', { x: x0, y, width: Math.max(2, x1 - x0), height: 24 }, `fill:${color}`)); acc += v; });
        kids.push(word(X(total) + 8, y + 16, Math.round(total) + ' MB', 'start', 'lbl val'));
      });
      [[BLUE, `weights: ${bits} bits each, ${Math.round(wMB)} MB`, 190], [GOLD, `group scales: 32 bits each, ${Math.round(sMB)} MB`, 400]].forEach(([color, label, x]) => kids.push(sv('rect', { x, y: 128, width: 10, height: 10 }, `fill:${color}`), word(x + 15, 137, label, 'start')));
      kids.push(sv('line', { x1: X(RAM), x2: X(RAM), y1: 14, y2: 118, class: 'ramline' }), word(X(RAM), 10, "iPod's RAM: 256 MB", 'middle', 'lbl meant'));
      svg.replaceChildren(...kids);
      note.textContent = `${bits} + 32/${g} = ${bpw.toFixed(2)} bits per weight, so ${Math.round(mb)} MB: ${(mb / f32 * 100).toFixed(0)}% of the floats, and ${mb > RAM ? 'more than the iPod has' : (mb / RAM * 100).toFixed(0) + '% of the iPod\'s RAM'}. Of that, the weights take ${Math.round(wMB)} MB and the group scales ${Math.round(sMB)} MB (${(32 / g).toFixed(2)} bits per weight).`;
    };
    fig.querySelectorAll('input').forEach((i) => i.addEventListener('input', draw)); draw();
  };

  // 12. Why the kernel rotates the activations: the identity with real numbers, and the operation count
  const reuseFig = (fig) => {
    const N = 8, box = $(fig, '.sx-reuse'), svg = $(fig, 'svg'), note = $(fig, '.sx-readout'); let seed = 5, w = [], x = [];
    const fw = (v) => { const y = v.slice(); for (let h = 1; h < N; h *= 2) for (let i = 0; i < N; i += 2 * h) for (let j = i; j < i + h; j++) { const a = y[j], b = y[j + h]; y[j] = a + b; y[j + h] = a - b; } return y.map((q) => q / Math.sqrt(N)); };
    const dot = (a, b) => a.reduce((t, q, i) => t + q * b[i], 0), sgn = (q, d) => (q < 0 ? '−' : '') + Math.abs(q).toFixed(d);
    const gen = () => { const r = rng2(seed), g = () => Math.sqrt(-2 * Math.log(r() + 1e-12)) * Math.cos(2 * Math.PI * r()); w = Array.from({ length: N }, g); w[Math.floor(r() * N)] += 3; x = Array.from({ length: N }, g); };
    const row = (label, v) => { const tr = el('tr'); tr.append(el('th', null, label)); v.forEach((a) => { const td = el('td', 'num', sgn(a, 2)); td.style.background = `color-mix(in srgb, ${a >= 0 ? 'var(--sx-red)' : 'var(--sx-blue)'} ${Math.min(100, Math.abs(a) / 3 * 100)}%, transparent)`; tr.append(td); }); return tr; };
    const result = (text) => { const tr = el('tr', 'res'), td = el('td', null, text); td.colSpan = N + 1; tr.append(td); return tr; };
    const draw = () => {
      const hw = fw(w), hx = fw(x), t = el('table');
      t.append(row('weights w', w), row('activations x', x), result(`w · x = ${sgn(dot(w, x), 4)}`), row('rotated weights Hw', hw), row('rotated activations Hx', hx), result(`(Hw) · (Hx) = ${sgn(dot(hw, hx), 4)}   the same answer`));
      box.replaceChildren(t);
      const R = 2 ** +$(fig, '[data-k=rows]').value, rot = 64 * 6, mac = 64, A = R * (rot + mac), B = rot + R * mac, fmt = (q) => q.toLocaleString('en-US');
      $(fig, '[data-o=rows]').textContent = fmt(R);
      const X = (q) => 200 + q / A * 340, kids = [], word = (px, py, tt, a, c) => { const e = sv('text', { x: px, y: py, 'text-anchor': a, class: c || 'lbl' }); e.textContent = tt; return e; };
      [['rotate every row\'s weights back', A, 'var(--sx-red)', 18], ['rotate the activations once', B, 'var(--sx-blue)', 66]].forEach(([name, q, color, y]) => kids.push(word(190, y + 17, name, 'end', 'lbl rowname'), sv('rect', { x: 200, y, width: Math.max(2, X(q) - 200), height: 26, rx: 3 }, `fill:${color}`), word(X(q) + 8, y + 17, fmt(q), 'start', 'lbl val')));
      svg.replaceChildren(...kids);
      note.textContent = `With ${fmt(R)} row${R > 1 ? 's' : ''}, rotating every row's weights back takes ${fmt(A)} operations, and rotating the activations once takes ${fmt(B)}: ${(A / B).toFixed(1)} times fewer. Each group of 64 needs 6 × 64 = ${rot} additions to rotate, and each row's dot product needs ${mac} multiply-adds.`;
    };
    $(fig, '[data-act=new]').addEventListener('click', () => { seed = (seed * 48271) % 2147483647; gen(); draw(); });
    $(fig, '[data-k=rows]').addEventListener('input', draw);
    gen(); draw();
  };

  const widgets = { inventory, repair, ace, ram, timeline, hadamard: hadamardFig, cq: cqFig, spread: spreadFig, fwht: fwhtFig, grouping: groupingFig, outlierloss: outlierLossFig, hist: histFig, rot2d: rot2dFig, rot3dg: rot3dgFig, lloyd: lloydFig, sizecalc: sizeCalcFig, reuse: reuseFig, pca: pcaFig, pca3d: pca3dFig, quantize: quantizeFig };
  document.querySelectorAll('figure.widget').forEach((fig) => { const f = widgets[fig.dataset.widget]; if (f) f(fig); });
  // Cross-references: a link with class "xref" scrolls to its target and raises a floating button that carries the reader back to the link
  (() => {
    const links = [...document.querySelectorAll('a.xref[href^="#"]')];
    if (!links.length) return;
    const reduce = matchMedia('(prefers-reduced-motion: reduce)').matches, behavior = reduce ? 'auto' : 'smooth';
    const btn = el('button', 'xref-back', '↩ Back to where you were'); btn.type = 'button'; btn.hidden = true; document.body.append(btn);
    let from = null, target = null, away = false, ticking = false;
    const seen = (e) => { const r = e.getBoundingClientRect(); return r.bottom > 0 && r.top < innerHeight; };
    const hide = () => { btn.hidden = true; from = target = null; away = false; };
    links.forEach((a) => a.addEventListener('click', (e) => {
      const t = document.getElementById(a.getAttribute('href').slice(1)); if (!t) return;
      e.preventDefault(); from = a; target = t; away = false; btn.hidden = false;
      t.scrollIntoView({ behavior, block: 'start' });
    }));
    btn.addEventListener('click', () => { const a = from; hide(); if (a) { a.scrollIntoView({ behavior, block: 'center' }); a.focus({ preventScroll: true }); } });
    addEventListener('scroll', () => {   // once the reader has reached the target and later sees the link again on their own, the button has done its job
      if (!from || ticking) return; ticking = true;
      requestAnimationFrame(() => { ticking = false; if (!from) return; if (seen(target)) away = true; else if (away && seen(from)) hide(); });
    }, { passive: true });
  })();
  // A contents column that floats on the left of wide windows: every numbered section, the 4-bit section's steps, and a highlight on the one being read
  (() => {
    const heads = [...document.querySelectorAll('.siri-explainer h2[id]:not(#contents), .siri-explainer h3[id], .siri-explainer h4[id], .siri-explainer [data-toc]')];
    if (!heads.length) return;
    const nav = el('nav', 'toc-float'); nav.setAttribute('aria-label', 'Contents'); nav.append(el('p', 'toc-title', 'Contents'));
    const ul = el('ul'), links = [];
    heads.forEach((h) => {
      const li = el('li', h.tagName === 'H2' ? '' : h.tagName === 'H4' ? 'subsub' : 'sub'), a = el('a', null, h.dataset.toc || h.textContent.trim());
      a.href = '#' + h.id; li.append(a); ul.append(li); links.push([h, a]);
    });
    nav.append(ul); document.body.append(nav);
    let active = null, ticking = false;
    const update = () => {
      ticking = false; let cur = links[0];
      for (const l of links) { if (l[0].getBoundingClientRect().top <= innerHeight * .3) cur = l; else break; }
      if (cur === active) return;
      if (active) active[1].removeAttribute('aria-current');
      active = cur; cur[1].setAttribute('aria-current', 'true');
      const r = cur[1].getBoundingClientRect(), n = nav.getBoundingClientRect();
      if (r.top < n.top + 40 || r.bottom > n.bottom - 20) nav.scrollTop += r.top - n.top - n.height / 2;     // keep the current entry in view when the column itself scrolls
    };
    addEventListener('scroll', () => { if (!ticking) { ticking = true; requestAnimationFrame(update); } }, { passive: true });
    addEventListener('resize', update); update();
  })();

  // LaTeX in the text: \( inline \) and \[ display \], rendered by KaTeX when it has loaded (the raw LaTeX stays readable if it hasn't)
  window.addEventListener('load', () => {
    if (window.renderMathInElement) renderMathInElement(document.body, { delimiters: [{ left: '\\[', right: '\\]', display: true }, { left: '\\(', right: '\\)', display: false }], throwOnError: false });
  });
})();
