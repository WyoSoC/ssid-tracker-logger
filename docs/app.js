'use strict';

// ── Map setup ──────────────────────────────────────────────────────────────

const map = L.map('map', { preferCanvas: true }).setView([39.5, -98.35], 5);

L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
  attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a> contributors',
  maxZoom: 19,
}).addTo(map);

// ── State ──────────────────────────────────────────────────────────────────

let allRecords     = [];   // parsed from CSV(s)
let ssidFrequency  = {};   // SSID name → location count
let markers        = [];   // Leaflet circle markers
let routeLine      = null; // Leaflet polyline
let activeFilter   = null; // currently highlighted SSID name

// ── File input ─────────────────────────────────────────────────────────────

document.getElementById('csvFile').addEventListener('change', function () {
  const files = Array.from(this.files);
  if (!files.length) return;

  showOverlay('Parsing…');
  allRecords   = [];
  ssidFrequency = {};

  let pending = files.length;
  files.forEach(file => {
    Papa.parse(file, {
      header: true,
      skipEmptyLines: true,
      complete(result) {
        parseRows(result.data);
        if (--pending === 0) finishLoad();
      },
    });
  });
});

function parseRows(rows) {
  rows.forEach(row => {
    const lat = parseFloat(row.lat);
    const lon = parseFloat(row.lon);
    if (isNaN(lat) || isNaN(lon)) return;

    const rawSsids = (row.ssids || '').replace(/^"|"$/g, '');
    const ssids = rawSsids.split(';').map(s => s.trim()).filter(Boolean);

    ssids.forEach(s => { ssidFrequency[s] = (ssidFrequency[s] || 0) + 1; });

    allRecords.push({ lat, lon, ssids, row });
  });
}

function finishLoad() {
  // Sort by timestamp so the route line is chronological
  allRecords.sort((a, b) => (a.row.local_time || '').localeCompare(b.row.local_time || ''));

  activeFilter = null;
  renderAll(allRecords);
  updateStats(allRecords);
  renderSsidList();

  // Show stats / legend
  ['statsCard', 'filterCard', 'legendCard'].forEach(id =>
    document.getElementById(id).classList.remove('hidden')
  );

  hideOverlay();
}

// ── Rendering ──────────────────────────────────────────────────────────────

function renderAll(records) {
  // Remove old markers and route
  markers.forEach(m => map.removeLayer(m));
  markers = [];
  if (routeLine) { map.removeLayer(routeLine); routeLine = null; }

  if (!records.length) return;

  // Route polyline (faint)
  const latlngs = records.map(r => [r.lat, r.lon]);
  routeLine = L.polyline(latlngs, { color: '#333', weight: 1.5, opacity: 0.7 }).addTo(map);

  // Markers
  records.forEach(rec => {
    const color  = ssidCountColor(rec.ssids.length);
    const marker = L.circleMarker([rec.lat, rec.lon], {
      radius:      6,
      fillColor:   color,
      color:       '#000',
      weight:      1,
      opacity:     0.9,
      fillOpacity: 0.85,
    });
    marker.bindPopup(() => buildPopup(rec), { maxWidth: 300 });
    marker.addTo(map);
    markers.push(marker);
  });

  // Fit bounds
  map.fitBounds(L.latLngBounds(latlngs).pad(0.08));
}

function ssidCountColor(n) {
  if (n === 0)  return '#555';
  if (n < 5)    return '#0af';
  if (n < 15)   return '#0f0';
  if (n < 30)   return '#ff0';
  return '#f80';
}

function buildPopup(rec) {
  const r = rec.row;
  const ssidHtml = rec.ssids.length
    ? rec.ssids.map(s => `<div class="popup-ssid">${esc(s)}</div>`).join('')
    : '<span style="color:#555">none detected</span>';

  return `
    <div class="popup-time">${esc(r.local_time || '—')}</div>
    <div class="popup-coords">${rec.lat.toFixed(5)}, ${rec.lon.toFixed(5)}</div>
    <div class="popup-ssid-header">${rec.ssids.length} SSIDs</div>
    <div class="popup-ssid-list">${ssidHtml}</div>
    <div class="popup-meta">
      GPS: ${r.satellites || '?'} sats &nbsp;·&nbsp; HDOP ${r.hdop || '?'} (${r.quality || '?'})<br>
      Battery: ${r.battery_v ? r.battery_v + ' V' : '—'}
    </div>`;
}

// ── Stats ──────────────────────────────────────────────────────────────────

function updateStats(records) {
  if (!records.length) return;

  set('statLocations', records.length);

  const allSsids = records.flatMap(r => r.ssids);
  set('statTotal', allSsids.length);
  set('statUnique', new Set(allSsids).size);

  const dates = records.map(r => r.row.local_time || '').filter(Boolean).sort();
  if (dates.length >= 2) {
    const d0 = dates[0].slice(0, 10);
    const d1 = dates[dates.length - 1].slice(0, 10);
    set('statDates', d0 === d1 ? d0 : `${d0} → ${d1}`);
  } else if (dates.length === 1) {
    set('statDates', dates[0].slice(0, 10));
  }

  // Approximate trip distance
  let km = 0;
  for (let i = 1; i < records.length; i++) {
    km += haversineKm(records[i-1].lat, records[i-1].lon, records[i].lat, records[i].lon);
  }
  set('statDist', km > 1 ? km.toFixed(1) + ' km' : (km * 1000).toFixed(0) + ' m');

  // Average battery
  const bvs = records.map(r => parseFloat(r.row.battery_v)).filter(v => !isNaN(v) && v > 0);
  if (bvs.length) {
    const avg = bvs.reduce((a, b) => a + b, 0) / bvs.length;
    set('statBattery', avg.toFixed(2) + ' V');
  } else {
    set('statBattery', '—');
  }
}

// ── SSID List ──────────────────────────────────────────────────────────────

function renderSsidList() {
  const sorted = Object.entries(ssidFrequency).sort((a, b) => b[1] - a[1]);
  document.getElementById('ssidCountBadge').textContent = sorted.length;

  const container = document.getElementById('ssidList');
  container.innerHTML = sorted.map(([name, count]) => `
    <div class="ssid-entry" data-ssid="${escAttr(name)}">
      <span class="ssid-name" title="${escAttr(name)}">${esc(name)}</span>
      <span class="ssid-count">×${count}</span>
    </div>`).join('');

  container.querySelectorAll('.ssid-entry').forEach(el => {
    el.addEventListener('click', () => {
      const ssid = el.dataset.ssid;
      if (activeFilter === ssid) {
        clearFilter();
      } else {
        applyFilter(ssid, el);
      }
    });
  });
}

function applyFilter(ssid, clickedEl) {
  activeFilter = ssid;
  document.querySelectorAll('.ssid-entry').forEach(el => el.classList.remove('active'));
  if (clickedEl) clickedEl.classList.add('active');

  const filtered = allRecords.filter(r => r.ssids.includes(ssid));
  renderAll(filtered);
  updateStats(filtered);

  if (filtered.length > 0) {
    map.fitBounds(
      L.latLngBounds(filtered.map(r => [r.lat, r.lon])).pad(0.15)
    );
  }
}

function clearFilter() {
  activeFilter = null;
  document.querySelectorAll('.ssid-entry').forEach(el => el.classList.remove('active'));
  renderAll(allRecords);
  updateStats(allRecords);
}

document.getElementById('resetFilterBtn').addEventListener('click', clearFilter);

document.getElementById('filterInput').addEventListener('input', function () {
  const q = this.value.toLowerCase();
  document.querySelectorAll('.ssid-entry').forEach(el => {
    el.style.display = el.dataset.ssid.toLowerCase().includes(q) ? '' : 'none';
  });
});

// ── Overlay ────────────────────────────────────────────────────────────────

function showOverlay(msg) {
  const el = document.getElementById('mapOverlay');
  document.getElementById('mapOverlayMsg').textContent = msg;
  el.classList.remove('hidden');
}

function hideOverlay() {
  document.getElementById('mapOverlay').classList.add('hidden');
}

// ── Utilities ──────────────────────────────────────────────────────────────

function set(id, val) {
  document.getElementById(id).textContent = val;
}

function haversineKm(lat1, lon1, lat2, lon2) {
  const R    = 6371;
  const dLat = (lat2 - lat1) * Math.PI / 180;
  const dLon = (lon2 - lon1) * Math.PI / 180;
  const a    = Math.sin(dLat / 2) ** 2
              + Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180)
              * Math.sin(dLon / 2) ** 2;
  return R * 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1 - a));
}

function esc(str) {
  return String(str)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}

function escAttr(str) {
  return String(str).replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}
