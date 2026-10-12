import { style } from './style.mjs';
export default class LowerThird extends HTMLElement {
  async load({ data }) {
    const root = this.attachShadow({ mode: 'open' });
    root.innerHTML = `<style>${style}</style><div class="bar"></div><div class="dot"></div>`;
    this.bar = root.querySelector('.bar');
    this.dot = root.querySelector('.dot');
    this.bar.style.background = data.color;
    fetch('https://example.com/x').catch(() => {});  // (offline: refused, not waited on)
  }
  async playAction() {
    this.bar.classList.add('in');
    const start = performance.now();
    const step = t => { this.dot.style.left = (t - start) / 10 + 'px'; requestAnimationFrame(step); };
    requestAnimationFrame(step);
    setTimeout(() => { this.dot.style.top = '20px'; }, 500);
    return {};
  }
  async stopAction() { this.bar.classList.remove('in'); return {}; }
  async updateAction({ data }) { this.bar.style.background = data.color; return {}; }
  async customAction() { return {}; }
  async dispose() { return {}; }
}
