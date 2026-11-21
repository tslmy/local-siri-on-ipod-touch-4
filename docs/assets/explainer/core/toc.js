// A contents column that floats on the left of wide windows, with a highlight on the section being read.
// `selector` picks the headings (or any element with an id); a `data-toc` attribute gives a shorter title.
import { el } from './dom.js';

export const mountToc = (selector) => {
  const heads = [...document.querySelectorAll(selector)];
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
};
