import { $, el, press } from '../core/dom.js';

// 5. Timeline from the end of speech to the full answer
export default (fig) => {
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
