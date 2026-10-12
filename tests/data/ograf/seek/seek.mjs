export default class Seek extends HTMLElement {
  async load(p) { this.mode = p.renderType; this.innerHTML = '<div style="position:absolute;left:0;top:0;height:10px;background:rgb(0,255,0)"></div>'; this.bar = this.firstChild; this.schedule = []; }
  async setActionsSchedule({ schedule }) { this.schedule = schedule; }
  async goToTime({ timestamp }) {
    if (this.mode !== 'non-realtime') throw new Error('not told non-realtime');
    this.bar.style.width = timestamp / 10 + 'px';
    const played = this.schedule.some(s => s.action.type === 'playAction' && s.timestamp <= timestamp);
    this.bar.style.background = played ? 'rgb(0, 0, 255)' : 'rgb(0, 255, 0)';
  }
  async playAction() { throw new Error('played in real time'); }
  async stopAction() {}
}
