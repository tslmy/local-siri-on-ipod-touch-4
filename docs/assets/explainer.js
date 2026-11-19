// Interactive figures for the local-siri explainer. No dependencies; each figure is a
// <figure class="widget" data-widget="..."> filled in here. Numbers are the ones measured on the iPod.
(() => {
  const $ = (root, sel) => root.querySelector(sel);
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  const press = (buttons, on) => buttons.forEach((b) => b.setAttribute('aria-pressed', String(b === on)));

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
          const terms = row.map((x, k) => `${x > 0 ? (k ? ' + ' : '') : (k ? ' − ' : '−')}x${k + 1}`).join('');
          const flips = row.slice(1).reduce((a, x, k) => a + (x !== row[k] ? 1 : 0), 0);
          note.textContent = `Output ${i + 1} = (${terms}) / √${n}. Every input counts equally, only the signs differ. The sign flips ${flips} times along this row: rows are like square waves of different frequencies, and any two rows agree in exactly half their places, which is what makes them orthogonal.`;
        });
        grid.append(c);
      }));
      box.replaceChildren(grid);
      note.textContent = n === 2
        ? 'The 2×2 matrix: output 1 is the sum of the two inputs, output 2 their difference (each divided by √2).'
        : `${n}×${n}: four copies of the ${n / 2}×${n / 2} matrix, the bottom-right one (outlined) with its signs flipped. Click a row.`;
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
      hr.append(el('th', null, `÷ √${N}`)); t.append(hr);
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
      note.textContent = k === 0
        ? `${N} activations. Multiplying by the 8×8 matrix directly would take 64 multiply-adds.`
        : k < ROUNDS
          ? `Round ${k}: pairs ${h} apart are combined. Blue rows got a + b, gold rows a − b.`
          : `Done after ${ROUNDS} rounds of ${N} additions or subtractions (${ROUNDS * N} in all, no multiplications), then one scaling. For a group of 64 it's 6 rounds: 384 operations instead of 4,096.`;
    };
    fig.querySelectorAll('button').forEach((b) => b.addEventListener('click', () => {
      if (b.dataset.act === 'next') k = k >= ROUNDS ? 0 : k + 1;
      if (b.dataset.act === 'reset') k = 0;
      draw();
    }));
    draw();
  };

  const widgets = { inventory, repair, ace, ram, timeline, hadamard: hadamardFig, cq: cqFig, fwht: fwhtFig };
  document.querySelectorAll('figure.widget').forEach((fig) => { const f = widgets[fig.dataset.widget]; if (f) f(fig); });
})();
