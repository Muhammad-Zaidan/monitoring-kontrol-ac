// ── CONFIG ───────────────────────────────────────────────────
const FIREBASE_HOST = "https://ac-monitor-industri-default-rtdb.asia-southeast1.firebasedatabase.app";
const FIREBASE_AUTH = "DcOLsJQyQptHH7fTBvQGHQ1ClZpU1Oh9CKWgrttM";

// ── STATE ────────────────────────────────────────────────────
let charts = {};
let historyData = { labels:[], temp:[], power:[], persons:[], hum:[], raw:[] };
const SPARK_MAX = 20;
const sparkData = { power:[], v1:[], v2:[], v3:[] };
let refreshCountdown = 15;
let refreshInterval = null;
let currentMode = 'auto'; // default mode
let manualTemp = 24; // local state for manual temp

// ── FIREBASE ─────────────────────────────────────────────────
async function fbGet(path) {
  const r = await fetch(`${FIREBASE_HOST}${path}.json?auth=${FIREBASE_AUTH}`);
  if (!r.ok) throw new Error(`Firebase error ${r.status}`);
  return r.json();
}

async function fbPut(path, data) {
  const r = await fetch(`${FIREBASE_HOST}${path}.json?auth=${FIREBASE_AUTH}`, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  });
  if (!r.ok) throw new Error(`Firebase PUT error ${r.status}`);
  return r.json();
}

async function fbPatch(path, data) {
  const r = await fetch(`${FIREBASE_HOST}${path}.json?auth=${FIREBASE_AUTH}`, {
    method: 'PATCH',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(data)
  });
  if (!r.ok) throw new Error(`Firebase PATCH error ${r.status}`);
  return r.json();
}

// ── CLOCK ────────────────────────────────────────────────────
function updateClock() {
  const now = new Date();
  document.getElementById('headerClock').textContent =
    now.toLocaleTimeString('id-ID', { hour:'2-digit', minute:'2-digit', second:'2-digit' });
}

// ── REFRESH TIMER ────────────────────────────────────────────
const CIRC = 2 * Math.PI * 14; // ~87.96

function startRefreshTimer() {
  refreshCountdown = 15;
  clearInterval(refreshInterval);
  updateRefreshUI();
  refreshInterval = setInterval(() => {
    refreshCountdown--;
    updateRefreshUI();
    if (refreshCountdown <= 0) {
      clearInterval(refreshInterval);
      fetchRealtime().then(() => startRefreshTimer());
    }
  }, 1000);
}

function updateRefreshUI() {
  const el = document.getElementById('refreshCount');
  const ring = document.getElementById('refreshRing');
  if (el) el.textContent = refreshCountdown;
  if (ring) ring.style.strokeDashoffset = CIRC * (1 - refreshCountdown / 15);
}

// ── SPARKLINES ───────────────────────────────────────────────
function pushSpark(key, val) {
  sparkData[key].push(val);
  if (sparkData[key].length > SPARK_MAX) sparkData[key].shift();
}

function renderSparkline(containerId, data, color) {
  const el = document.getElementById(containerId);
  if (!el || data.length < 2) return;
  const w = 80, h = 28;
  const max = Math.max(...data), min = Math.min(...data);
  const range = max - min || 1;
  const pts = data.map((v, i) => {
    const x = (i / (data.length - 1)) * w;
    const y = h - 2 - ((v - min) / range) * (h - 4);
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  el.innerHTML = `<svg width="${w}" height="${h}" viewBox="0 0 ${w} ${h}"><polyline points="${pts.join(' ')}" fill="none" stroke="${color}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>`;
}

// ── ANIMATE VALUE ────────────────────────────────────────────
function animateValue(elId, newVal, unit, dec) {
  const el = document.getElementById(elId);
  if (!el) return;
  const from = parseFloat(el.dataset.val) || 0;
  const to = parseFloat(newVal) || 0;
  el.dataset.val = to;
  const dur = 400;
  const start = performance.now();
  function tick(now) {
    const p = Math.min((now - start) / dur, 1);
    const ease = 1 - Math.pow(1 - p, 3);
    const v = from + (to - from) * ease;
    el.innerHTML = v.toFixed(dec) + `<span class="card-unit">${unit}</span>`;
    if (p < 1) requestAnimationFrame(tick);
  }
  requestAnimationFrame(tick);
}

// ── INIT CHARTS ──────────────────────────────────────────────
const chartCfg = {
  responsive: true, maintainAspectRatio: false, animation: { duration: 400 },
  plugins: {
    legend: { display: false },
    tooltip: {
      backgroundColor:'#fff', borderColor:'#e3e8ef', borderWidth:1,
      titleFont:{family:'Inter',size:11,weight:'600'}, bodyFont:{family:'JetBrains Mono',size:12},
      titleColor:'#475569', bodyColor:'#0f172a',
      padding:10, cornerRadius:8,
      boxShadow:'0 4px 12px rgba(0,0,0,0.1)'
    }
  },
  scales: {
    x: { grid:{color:'rgba(227,232,239,0.6)'}, ticks:{color:'#94a3b8',font:{family:'Inter',size:10,weight:'500'},maxTicksLimit:10} },
    y: { grid:{color:'rgba(227,232,239,0.6)'}, ticks:{color:'#94a3b8',font:{family:'Inter',size:10,weight:'500'}} }
  }
};

function makeChart(id, color, bgAlpha) {
  const bg = color + (bgAlpha || '18');
  return new Chart(document.getElementById(id).getContext('2d'), {
    type: 'line',
    data: { labels:[], datasets:[{ data:[], borderColor:color, backgroundColor:bg,
      borderWidth:2, pointRadius:1.5, pointHoverRadius:5, fill:true, tension:0.4 }] },
    options: { ...chartCfg }
  });
}

function initCharts() {
  charts.temp    = makeChart('chartTemp',    '#ef4444', '14');
  charts.power   = makeChart('chartPower',   '#3b82f6', '14');
  charts.persons = makeChart('chartPersons', '#10b981', '14');
  charts.hum     = makeChart('chartHum',     '#f59e0b', '14');
}

function updateCharts(labels, temp, power, persons, hum) {
  const max = 150;
  const step = Math.max(1, Math.floor(labels.length / max));
  const f = arr => arr.filter((_, i) => i % step === 0);
  const fl = f(labels);
  charts.temp.data.labels    = fl; charts.temp.data.datasets[0].data    = f(temp);
  charts.power.data.labels   = fl; charts.power.data.datasets[0].data   = f(power);
  charts.persons.data.labels = fl; charts.persons.data.datasets[0].data = f(persons);
  charts.hum.data.labels     = fl; charts.hum.data.datasets[0].data     = f(hum);
  [charts.temp, charts.power, charts.persons, charts.hum].forEach(c => c.update());
}

// ── REALTIME ─────────────────────────────────────────────────
function renderSlaves(data) {
  const grid = document.getElementById('slaveGrid');
  const slaves = Object.entries(data).filter(([k]) => k.startsWith('Slave'));
  if (!slaves.length) {
    grid.innerHTML = '<div class="slave-card"><div class="slave-header"><div class="slave-name">Belum ada slave terhubung</div></div></div>';
    return;
  }
  grid.innerHTML = slaves.map(([name, s]) => `
    <div class="slave-card">
      <div class="slave-header">
        <div class="slave-name">${name.toUpperCase()}</div>
        <div class="badge-group">
          <div class="badge ${s.online ? 'online' : 'offline'}">${s.online ? 'ONLINE' : 'OFFLINE'}</div>
          <div class="badge ${s.acState ? 'ac-on' : 'ac-off'}">${s.acState ? 'AC ON' : 'AC OFF'}</div>
        </div>
      </div>
      <div class="slave-metrics">
        <div class="metric"><div class="metric-val ${(s.temp||0)>28?'hot':'cool'}">${(s.temp||0).toFixed(1)}</div><div class="metric-lbl">Suhu °C</div></div>
        <div class="metric"><div class="metric-val">${(s.hum||0).toFixed(1)}</div><div class="metric-lbl">Lembab %</div></div>
        <div class="metric"><div class="metric-val green">${s.n??'--'}</div><div class="metric-lbl">Orang</div></div>
        <div class="metric"><div class="metric-val cool">${s.tset??'--'}</div><div class="metric-lbl">Tset °C</div></div>
        <div class="metric"><div class="metric-val">${s.cards??'--'}</div><div class="metric-lbl">Kartu</div></div>
      </div>
    </div>`).join('');
}

function renderPzem(data) {
  let total = 0;
  [1,2,3].forEach(i => { const p = data['pzem'+i]; if(p) total += (p.p||0); });

  // Sparkline data
  pushSpark('power', total);
  for (let i=1;i<=3;i++) { const p=data['pzem'+i]; if(p) pushSpark('v'+i, p.v||0); }

  // Animated values
  animateValue('totalPower', total, 'W', 1);
  for (let i=1;i<=3;i++) { const p=data['pzem'+i]; if(p) animateValue('v'+i, p.v||0, 'V', 1); }

  // Sparklines
  renderSparkline('sparkPower', sparkData.power, '#f97316');
  renderSparkline('sparkV1', sparkData.v1, '#10b981');
  renderSparkline('sparkV2', sparkData.v2, '#10b981');
  renderSparkline('sparkV3', sparkData.v3, '#10b981');

  // Table
  document.getElementById('pzemTable').innerHTML = [1,2,3].map(i => {
    const p = data['pzem'+i]||{};
    return `<tr>
      <td class="td-channel">CH${i}</td>
      <td class="td-v">${(p.v||0).toFixed(1)}</td>
      <td class="td-i">${(p.i||0).toFixed(2)}</td>
      <td class="td-p">${(p.p||0).toFixed(1)}</td>
      <td class="td-pf">${(p.pf||0).toFixed(2)}</td>
      <td class="td-e">${(p.e||0).toFixed(3)}</td>
    </tr>`;
  }).join('');
}

async function fetchRealtime() {
  try {
    const data = await fbGet('/realtime');
    if (!data) return;

    const masterTs = data._masterTs || 0;
    const nowSec = Math.floor(Date.now() / 1000);
    const masterOffline = masterTs > 0 && (nowSec - masterTs) > 60;

    if (masterOffline) {
      const grid = document.getElementById('slaveGrid');
      const cards = grid.querySelectorAll('.badge');
      cards.forEach(b => { b.className = 'badge offline'; b.textContent = 'OFFLINE'; });
      document.getElementById('wifiDot').className = 'dot offline';
      document.getElementById('wifiStatus').textContent = 'Master Offline';
      return;
    }

    renderSlaves(data);
    renderPzem(data);

    // Sync mode dari Firebase realtime
    if (data.mode) {
      currentMode = data.mode;
      updateControlUI();
    }

    // Sync manual temp dari slave pertama (sebagai acuan)
    let firstSlaveKey = Object.keys(data).find(k => k !== 'pzem1' && k !== 'pzem2' && k !== 'pzem3' && !k.startsWith('_') && k !== 'mode');
    if (firstSlaveKey && data[firstSlaveKey].tset) {
      // Hanya update manualTemp jika tidak ada delay perubahan manual yang sedang berjalan
      if (!window.manualTempAdjusting) {
        manualTemp = data[firstSlaveKey].tset;
        const tempEl = document.getElementById('manualTempVal');
        if (tempEl) tempEl.textContent = manualTemp;
      }
    }

    document.getElementById('lastUpdate').textContent = 'Update: ' + new Date().toLocaleTimeString('id-ID');
    document.getElementById('wifiDot').className = 'dot';
    document.getElementById('wifiStatus').textContent = 'Terhubung';
    document.getElementById('errorBanner').classList.remove('show');
  } catch(e) {
    document.getElementById('wifiDot').className = 'dot offline';
    document.getElementById('wifiStatus').textContent = 'Terputus';
    document.getElementById('errorBanner').textContent = 'Gagal mengambil data: ' + e.message;
    document.getElementById('errorBanner').classList.add('show');
  }
}

// ── HISTORY ──────────────────────────────────────────────────
function getDatesInRange(from, to) {
  const dates = [], cur = new Date(from);
  const end = new Date(to);
  while (cur <= end) {
    dates.push(cur.toISOString().split('T')[0]);
    cur.setDate(cur.getDate() + 1);
  }
  return dates;
}

function getPresetDates(preset) {
  const now = new Date();
  const to = now.toISOString().split('T')[0];
  const from = new Date(now);
  if (preset === '1d') from.setDate(from.getDate() - 0);
  else if (preset === '1w') from.setDate(from.getDate() - 6);
  else if (preset === '1m') from.setDate(from.getDate() - 29);
  return { from: from.toISOString().split('T')[0], to };
}

async function loadHistoryRange(fromDate, toDate, isPreset) {
  document.getElementById('chartLoading').style.display = 'inline-block';
  const dates = getDatesInRange(fromDate, toDate);
  const labels=[], tempD=[], powerD=[], personsD=[], humD=[], rawRows=[];

  for (const date of dates) {
    try {
      const day = await fbGet(`/history/${date}`);
      if (!day) continue;
      const times = Object.keys(day).sort();
      for (const time of times) {
        const e = day[time];
        const label = isPreset && dates.length === 1 ? time : date.slice(5) + ' ' + time;
        labels.push(label);
        const sk = Object.keys(e).find(k => k.startsWith('Slave'));
        const temp = sk ? (e[sk].temp||0) : null;
        const hum  = sk ? (e[sk].hum||0)  : null;
        const pers = sk ? (e[sk].n||0)    : null;
        tempD.push(temp); humD.push(hum); personsD.push(pers);
        let tp = 0;
        ['pzem1','pzem2','pzem3'].forEach(k => { if(e[k]) tp += e[k].p||0; });
        powerD.push(tp);
        rawRows.push({
          waktu: date + ' ' + time,
          suhu: temp, kelembaban: hum, orang: pers, daya_total: tp.toFixed(1),
          v1: e.pzem1?.v||0, i1: e.pzem1?.i||0, p1: e.pzem1?.p||0,
          v2: e.pzem2?.v||0, i2: e.pzem2?.i||0, p2: e.pzem2?.p||0,
          v3: e.pzem3?.v||0, i3: e.pzem3?.i||0, p3: e.pzem3?.p||0,
        });
      }
    } catch(e) { /* skip */ }
  }

  historyData = { labels, temp: tempD, power: powerD, persons: personsD, hum: humD, raw: rawRows };
  updateCharts(labels, tempD, powerD, personsD, humD);
  document.getElementById('chartLoading').style.display = 'none';
}

async function changePreset(preset, btn) {
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  const { from, to } = getPresetDates(preset);
  document.getElementById('dateFrom').value = from;
  document.getElementById('dateTo').value = to;
  await loadHistoryRange(from, to, true);
}

async function applyDateRange() {
  const from = document.getElementById('dateFrom').value;
  const to   = document.getElementById('dateTo').value;
  if (!from || !to) { alert('Pilih tanggal dari dan sampai'); return; }
  if (from > to) { alert('Tanggal awal tidak boleh lebih besar dari tanggal akhir'); return; }
  document.querySelectorAll('.filter-btn').forEach(b => b.classList.remove('active'));
  await loadHistoryRange(from, to, false);
}

// ── EXPORT PNG ───────────────────────────────────────────────
function exportChartPNG(canvasId, name) {
  const canvas = document.getElementById(canvasId);
  const link = document.createElement('a');
  link.download = `kontrol-ac-${name}-${new Date().toISOString().slice(0,10)}.png`;
  link.href = canvas.toDataURL('image/png');
  link.click();
}

async function exportAllChartsPNG() {
  const from = document.getElementById('exportFrom').value || document.getElementById('dateFrom').value || new Date().toISOString().slice(0,10);
  const to   = document.getElementById('exportTo').value   || document.getElementById('dateTo').value   || from;
  const names = [
    {id:'chartTemp',name:'suhu'},{id:'chartPower',name:'daya'},
    {id:'chartPersons',name:'orang'},{id:'chartHum',name:'kelembaban'},
  ];
  for (const {id, name} of names) {
    await new Promise(r => setTimeout(r, 200));
    exportChartPNG(id, `${name}-${from}-${to}`);
  }
}

// ── EXPORT XLSX ──────────────────────────────────────────────
async function exportXLSX() {
  const from = document.getElementById('exportFrom').value;
  const to   = document.getElementById('exportTo').value;
  if (!from || !to) { alert('Pilih rentang tanggal terlebih dahulu'); return; }
  if (historyData.raw.length === 0) await loadHistoryRange(from, to, false);

  const wb = XLSX.utils.book_new();
  const wsData = historyData.raw.map(r => ({
    'Waktu': r.waktu, 'Suhu (°C)': r.suhu, 'Kelembaban (%)': r.kelembaban,
    'Jumlah Orang': r.orang, 'Daya Total (W)': r.daya_total,
    'V CH1 (V)': r.v1, 'I CH1 (A)': r.i1, 'P CH1 (W)': r.p1,
    'V CH2 (V)': r.v2, 'I CH2 (A)': r.i2, 'P CH2 (W)': r.p2,
    'V CH3 (V)': r.v3, 'I CH3 (A)': r.i3, 'P CH3 (W)': r.p3,
  }));
  const ws1 = XLSX.utils.json_to_sheet(wsData);
  ws1['!cols'] = Array(14).fill({ wch: 16 });
  XLSX.utils.book_append_sheet(wb, ws1, 'Data Historis');

  const vals = historyData.raw;
  const avg = arr => arr.length ? (arr.reduce((a,b)=>a+(b||0),0)/arr.length).toFixed(2) : '-';
  const mx = arr => arr.length ? Math.max(...arr.filter(x=>x!=null)).toFixed(2) : '-';
  const mn = arr => arr.length ? Math.min(...arr.filter(x=>x!=null)).toFixed(2) : '-';
  const temps = vals.map(r=>r.suhu).filter(x=>x!=null);
  const pows  = vals.map(r=>parseFloat(r.daya_total)).filter(x=>!isNaN(x));
  const hums  = vals.map(r=>r.kelembaban).filter(x=>x!=null);

  const wsSummary = XLSX.utils.json_to_sheet([
    { Parameter: 'Periode', Nilai: `${from} s/d ${to}` },
    { Parameter: 'Jumlah Data', Nilai: vals.length },
    { Parameter: 'Suhu Rata-rata (°C)', Nilai: avg(temps) },
    { Parameter: 'Suhu Maks (°C)', Nilai: mx(temps) },
    { Parameter: 'Suhu Min (°C)', Nilai: mn(temps) },
    { Parameter: 'Kelembaban Rata-rata (%)', Nilai: avg(hums) },
    { Parameter: 'Daya Rata-rata (W)', Nilai: avg(pows) },
    { Parameter: 'Daya Maks (W)', Nilai: mx(pows) },
    { Parameter: 'Diberikan oleh', Nilai: 'KONTROL AC — Sistem Monitoring Industri' },
    { Parameter: 'Diekspor pada', Nilai: new Date().toLocaleString('id-ID') },
  ]);
  wsSummary['!cols'] = [{ wch: 30 }, { wch: 40 }];
  XLSX.utils.book_append_sheet(wb, wsSummary, 'Ringkasan');
  XLSX.writeFile(wb, `kontrol-ac-${from}-${to}.xlsx`);
}

// ── EXPORT PDF ───────────────────────────────────────────────
async function exportPDF() {
  const from = document.getElementById('exportFrom').value;
  const to   = document.getElementById('exportTo').value;
  if (!from || !to) { alert('Pilih rentang tanggal terlebih dahulu'); return; }
  if (historyData.raw.length === 0) await loadHistoryRange(from, to, false);

  const { jsPDF } = window.jspdf;
  const doc = new jsPDF({ orientation: 'landscape', unit: 'mm', format: 'a4' });

  // Header - Light theme
  doc.setFillColor(59, 130, 246);
  doc.rect(0, 0, 297, 28, 'F');
  doc.setTextColor(255, 255, 255);
  doc.setFontSize(18);
  doc.setFont('helvetica', 'bold');
  doc.text('KONTROL AC', 14, 12);
  doc.setFontSize(9);
  doc.setTextColor(219, 234, 254);
  doc.text('Sistem Monitoring Industri', 14, 18);
  doc.setTextColor(255, 255, 255);
  doc.text(`Laporan Periode: ${from} s/d ${to}`, 14, 24);
  doc.text(`Diekspor: ${new Date().toLocaleString('id-ID')}`, 200, 24);

  const vals = historyData.raw;
  const avg = arr => arr.length ? (arr.reduce((a,b)=>a+(b||0),0)/arr.length).toFixed(1) : '-';
  const temps = vals.map(r=>r.suhu).filter(x=>x!=null);
  const pows  = vals.map(r=>parseFloat(r.daya_total)).filter(x=>!isNaN(x));
  const hums  = vals.map(r=>r.kelembaban).filter(x=>x!=null);

  doc.setFontSize(11);
  doc.setTextColor(59, 130, 246);
  doc.text('Ringkasan', 14, 36);

  doc.autoTable({
    startY: 40,
    head: [['Parameter', 'Nilai']],
    body: [
      ['Jumlah Data', vals.length],
      ['Suhu Rata-rata', avg(temps) + ' °C'],
      ['Kelembaban Rata-rata', avg(hums) + ' %'],
      ['Daya Rata-rata', avg(pows) + ' W'],
    ],
    styles: { fontSize: 9, cellPadding: 3, textColor: [15, 23, 42], fillColor: [248, 250, 252] },
    headStyles: { fillColor: [59, 130, 246], textColor: [255, 255, 255], fontStyle: 'bold' },
    alternateRowStyles: { fillColor: [241, 245, 249] },
    tableWidth: 100, margin: { left: 14 },
  });

  doc.addPage();
  doc.setFillColor(59, 130, 246);
  doc.rect(0, 0, 297, 14, 'F');
  doc.setFontSize(11);
  doc.setTextColor(255, 255, 255);
  doc.text('Data Historis', 14, 10);

  const maxRows = Math.min(vals.length, 500);
  const tableRows = vals.slice(0, maxRows).map(r => [
    r.waktu, r.suhu??'-', r.kelembaban??'-', r.orang??'-', r.daya_total,
    r.v1.toFixed(1), r.p1.toFixed(1), r.v2.toFixed(1), r.p2.toFixed(1), r.v3.toFixed(1), r.p3.toFixed(1),
  ]);

  doc.autoTable({
    startY: 16,
    head: [['Waktu','Suhu(°C)','Lembab(%)','Orang','Daya(W)','V1','P1','V2','P2','V3','P3']],
    body: tableRows,
    styles: { fontSize: 7, cellPadding: 2, textColor: [15, 23, 42], fillColor: [255, 255, 255] },
    headStyles: { fillColor: [59, 130, 246], textColor: [255, 255, 255], fontStyle: 'bold' },
    alternateRowStyles: { fillColor: [248, 250, 252] },
    margin: { left: 8, right: 8 },
  });

  if (vals.length > 500) {
    doc.setFontSize(8);
    doc.setTextColor(148, 163, 184);
    doc.text(`* Menampilkan 500 dari ${vals.length} baris. Export XLSX untuk data lengkap.`, 8, doc.lastAutoTable.finalY + 6);
  }

  doc.save(`kontrol-ac-${from}-${to}.pdf`);
}

// ── MODE & CONTROL ──────────────────────────────────────────
function updateControlUI() {
  const isManual = currentMode === 'manual';
  const btnAuto = document.getElementById('btnAuto');
  const btnManual = document.getElementById('btnManual');
  const controls = document.getElementById('manualControls');
  const status = document.getElementById('modeStatus');

  if (btnAuto) btnAuto.classList.toggle('active', !isManual);
  if (btnManual) btnManual.classList.toggle('active', isManual);

  // Enable/disable manual controls
  ['ctrlOn', 'ctrlOff', 'ctrlUp', 'ctrlDown'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.disabled = !isManual;
  });

  if (controls) {
    if (isManual) {
      controls.classList.add('show');
    } else {
      controls.classList.remove('show');
    }
  }

  if (status) {
    if (isManual) {
      status.textContent = 'Mode: Manual — AC dikontrol dari website';
      status.style.color = 'var(--warning)';
    } else {
      status.textContent = 'Mode: Auto — AC dikontrol otomatis berdasarkan sensor';
      status.style.color = 'var(--text-dim)';
    }
  }
}

async function setMode(mode) {
  currentMode = mode;
  updateControlUI();
  try {
    await fbPatch('/control', { mode: mode });
    console.log(`[MODE] Set ke ${mode}`);
  } catch(e) {
    console.error('Gagal set mode:', e);
    alert('Gagal mengubah mode. Coba lagi.');
  }
}

async function sendCmd(cmd) {
  if (currentMode !== 'manual') {
    alert('Ubah ke mode MANUAL terlebih dahulu!');
    return;
  }
  
  // Local UI prediction for faster feedback
  if (cmd === 'temp_up') {
    manualTemp = Math.min(30, manualTemp + 1);
    document.getElementById('manualTempVal').textContent = manualTemp;
    window.manualTempAdjusting = true;
    setTimeout(() => { window.manualTempAdjusting = false; }, 4000);
  } else if (cmd === 'temp_down') {
    manualTemp = Math.max(16, manualTemp - 1);
    document.getElementById('manualTempVal').textContent = manualTemp;
    window.manualTempAdjusting = true;
    setTimeout(() => { window.manualTempAdjusting = false; }, 4000);
  }

  // Disable tombol sementara
  ['ctrlOn', 'ctrlOff', 'ctrlUp', 'ctrlDown'].forEach(id => {
    const el = document.getElementById(id);
    if (el) el.disabled = true;
  });
  
  try {
    await fbPatch('/control', { cmd: cmd, cmdTs: Date.now() });
    console.log(`[CMD] Kirim: ${cmd}`);
    
    // Feedback visual singkat
    const btnMap = { ac_on: 'ctrlOn', ac_off: 'ctrlOff', temp_up: 'ctrlUp', temp_down: 'ctrlDown' };
    const btnId = btnMap[cmd];
    if (btnId) {
      const btn = document.getElementById(btnId);
      if (btn) {
        btn.classList.add('pulse');
        setTimeout(() => btn.classList.remove('pulse'), 600);
      }
    }
  } catch(e) {
    console.error('Gagal kirim perintah:', e);
    alert('Gagal mengirim perintah. Coba lagi.');
  }
  
  // Re-enable setelah jeda singkat
  setTimeout(() => {
    if (currentMode === 'manual') {
      ['ctrlOn', 'ctrlOff', 'ctrlUp', 'ctrlDown'].forEach(id => {
        const el = document.getElementById(id);
        if (el) el.disabled = false;
      });
    }
  }, 1000);
}

// ── INIT ─────────────────────────────────────────────────────
async function init() {
  initCharts();
  updateControlUI(); // Set initial UI state

  // Clock
  updateClock();
  setInterval(updateClock, 1000);

  // Set default dates
  const today = new Date().toISOString().split('T')[0];
  const weekAgo = new Date(); weekAgo.setDate(weekAgo.getDate()-6);
  const weekAgoStr = weekAgo.toISOString().split('T')[0];
  document.getElementById('dateFrom').value = today;
  document.getElementById('dateTo').value   = today;
  document.getElementById('exportFrom').value = weekAgoStr;
  document.getElementById('exportTo').value   = today;

  // Load mode awal dari Firebase
  try {
    const ctrl = await fbGet('/control');
    if (ctrl && ctrl.mode) {
      currentMode = ctrl.mode;
      updateControlUI();
    }
  } catch(e) { /* mode default auto */ }

  await fetchRealtime();
  await loadHistoryRange(today, today, true);

  setTimeout(() => document.getElementById('loadingOverlay').classList.add('hidden'), 1600);

  // Start auto-refresh with visual timer (replaces setInterval)
  startRefreshTimer();
}

init();
