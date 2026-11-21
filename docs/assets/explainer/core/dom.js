// Small DOM and SVG helpers shared by every figure.
export const $ = (root, sel) => root.querySelector(sel);
export const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
export const SVGNS = 'http://www.w3.org/2000/svg';
export const sv = (tag, attrs, style) => { const e = document.createElementNS(SVGNS, tag); Object.entries(attrs).forEach(([k, v]) => e.setAttribute(k, v)); if (style) e.style.cssText = style; return e; };
export const press = (buttons, on) => buttons.forEach((b) => b.setAttribute('aria-pressed', String(b === on)));
// An SVG <text> (anchor: start, middle or end; the class defaults to the muted label style) and an SVG <line>.
export const svgText = (x, y, str, anchor = 'start', cls = 'lbl', style) => { const t = sv('text', { x, y, 'text-anchor': anchor, class: cls }, style); t.textContent = str; return t; };
export const svgLine = (x1, y1, x2, y2, cls, style) => sv('line', { x1, y1, x2, y2, ...(cls ? { class: cls } : {}) }, style);
// Makes marks that react to clicks reachable by keyboard: focusable, announced as buttons, and activated by Enter or Space.
export const focusable = (root, selector, label) => {
  root.querySelectorAll(selector).forEach((e, i) => { e.setAttribute('tabindex', '0'); e.setAttribute('role', 'button'); e.setAttribute('aria-label', label(e, i)); });
  if (root.dataset.kbd) return;
  root.dataset.kbd = '1';
  root.addEventListener('keydown', (ev) => { if ((ev.key === 'Enter' || ev.key === ' ') && ev.target.matches?.('[role=button]')) { ev.preventDefault(); ev.target.dispatchEvent(new MouseEvent('click', { bubbles: true })); } });
};
