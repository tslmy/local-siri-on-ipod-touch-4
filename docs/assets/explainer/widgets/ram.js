import { $, el } from '../core/dom.js';

// 4. RAM budget during a question
export default (fig) => {
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
