// Entry point of the project page: fills every <figure class="widget" data-widget="..."> with its figure (loaded on demand)
// and adds the floating contents column. The helpers in core/ are shared with the blog (see scripts/sync-explainer-core.sh).
import { mountToc } from './core/toc.js';

const widgets = {
  inventory: () => import('./widgets/inventory.js'),
  repair: () => import('./widgets/repair.js'),
  ace: () => import('./widgets/ace.js'),
  ram: () => import('./widgets/ram.js'),
  timeline: () => import('./widgets/timeline.js'),
};

document.querySelectorAll('figure.widget[data-widget]').forEach((fig) => {
  const load = widgets[fig.dataset.widget];
  if (load) load().then((m) => m.default(fig));
});
mountToc('.explainer h2[id]:not(#contents), .explainer h3[id], .explainer h4[id], .explainer [data-toc]');
