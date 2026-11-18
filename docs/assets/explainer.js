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
      ['ipod', 'AVVoiceController', 'recording', 'Records and endpoints your voice. Present, but the iPod has no Speex encoder, see section 5.'],
      ['ipod', 'MobileGestalt', 'says "no Siri here"', 'The capability database. It answers false for the "assistant" key, so the tweak hooks it (section 4).'],
      ['file', 'assistantd', 'the Siri daemon', 'A normal executable in AssistantServices.framework on the 4S. It records audio, keeps the ACE session with the server, and relays results to SpringBoard. All 35 libraries it links are on the iPod.'],
      ['file', 'Assistant.uibundle', 'SpringBoard plugin', 'The Siri screen itself: the paper-texture bubbles, the glowing microphone. A loadable bundle, not in the cache.'],
      ['file', '/System/Library/Assistant', 'UI plugins', 'Snippet plugins for maps, weather, stocks, reminders and so on. They load, but this project never sends their objects.'],
      ['file', 'Settings, LaunchDaemons', 'and resources', 'Siri\'s page in Settings, the launchd jobs for assistantd and assistant_service, and the frameworks\' resource folders: about 45 MB in all.'],
      ['cache', 'AssistantUI.framework', '137 KB, repaired', 'The only binary that exists solely inside the 4S\'s shared cache. Extracting it and making it load elsewhere is section 3.'],
      ['ours', 'SiriCapability tweak', 'Substrate', 'Says yes to the capability check, records PCM instead of Speex, and disables timeouts sized for Apple\'s servers.'],
      ['ours', 'acesrv', 'ACE server', 'Stands in for guzzoni.apple.com on 127.0.0.1:4443 (section 6).'],
      ['ours', 'localsirid', 'speech + LLM', 'Moonshine Tiny and SmolLM2-135M in C and NEON (section 8).'],
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
      ['c', 'SpeechPacket × 12', 'Your voice: PCM or Speex, whatever the label says. acesrv decides from the size of the first packet.'],
      ['c', 'Chunk', 'A sync chunk arrives in the middle of the speech.'],
      ['s', 'ChunkAccepted', 'acesrv accepts every chunk, echoing its new generation.'],
      ['c', 'FinishSpeech', 'Siri\'s endpointer decided you stopped. Audio goes to localsirid on a worker thread; the main loop keeps answering pings.'],
      ['s', 'SpeechRecognized  "What is the capital of France?"', 'Sent as soon as Moonshine finishes, about 2.5 s later, so your words appear while the language model works.'],
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
    const state = { siri: true, prefetch: true };
    const bar = $(fig, '.sx-bar'), note = $(fig, '.sx-note');
    const draw = () => {
      const parts = [
        ['wired: kernel, drivers', 46, '#6b6b6b'],
        ['daemons', 34, '#8a8a8a'],
        ['SpringBoard', 20, '#2f6db3'],
        ...(state.siri ? [['Siri: UI, assistantd, voice', 25, '#7a4fd0']] : []),
        ['other file cache', 20, '#a0a0a0'],
        ['Moonshine', 30, '#c88d0c'],
        ['SmolLM2 working set', 80, '#2e7d4f'],
      ];
      const total = parts.reduce((s, p) => s + p[1], 0), over = Math.max(0, total - 256);
      bar.replaceChildren(...parts.map(([name, mb, c]) => {
        const d = el('div', null, `${name} ${mb}`); d.style.flex = `0 0 ${(mb / Math.max(256, total)) * 100}%`; d.style.background = c; d.title = `${name}: ~${mb} MB`; return d;
      }));
      if (over) { const d = el('div', 'over', ''); d.style.flex = `0 0 ${(over / total) * 100}%`; d.title = `${over} MB more than the iPod has`; bar.append(d); }
      const speed = state.prefetch ? 80 : 6, reload = 80 / speed;
      note.textContent = over
        ? `Needs ~${total} MB of 256: the models can't both stay resident, so each stage evicts the other. Bringing SmolLM2's 80 MB back takes ~${reload.toFixed(reload < 2 ? 1 : 0)} s at ${speed} MB/s${state.prefetch ? ' (one sequential read)' : ' (one page fault at a time)'}.`
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

  const widgets = { inventory, repair, ace, ram, timeline };
  document.querySelectorAll('figure.widget').forEach((fig) => { const f = widgets[fig.dataset.widget]; if (f) f(fig); });
})();
