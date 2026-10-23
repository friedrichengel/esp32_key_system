/* esp32_key_system – Web-UI (ohne Framework) */
(function () {
  'use strict';
  const $ = (s, r) => (r || document).querySelector(s);
  const $$ = (s, r) => Array.from((r || document).querySelectorAll(s));
  let session = { authenticated: false, csrf: null };
  let refreshTimer = null;

  /* ---------- API ---------- */
  async function api(path, opts) {
    opts = opts || {};
    const headers = Object.assign({}, opts.headers || {});
    if (opts.method && opts.method !== 'GET' && session.csrf) headers['X-CSRF-Token'] = session.csrf;
    if (opts.json !== undefined) { headers['Content-Type'] = 'application/json'; opts.body = JSON.stringify(opts.json); }
    const res = await fetch(path, { method: opts.method || 'GET', headers, body: opts.body, credentials: 'same-origin' });
    let data = null;
    try { data = await res.json(); } catch (e) { data = null; }
    if (res.status === 401 && !path.endsWith('/login')) { session = { authenticated: false }; route(); }
    if (!res.ok) throw new Error((data && data.error) || ('HTTP ' + res.status));
    return data;
  }

  function toast(msg, cls) {
    const t = $('#toast');
    t.textContent = msg; t.className = 'toast ' + (cls || '');
    clearTimeout(t._h); t._h = setTimeout(() => t.classList.add('hidden'), 4000);
  }

  function kv(el, rows) {
    el.innerHTML = rows.map(r => `<div class="k">${r[0]}</div><div class="v ${r[2] || ''}">${r[1] == null || r[1] === '' ? '–' : r[1]}</div>`).join('');
  }
  function esc(s) { return String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c])); }
  function fmtTs(e) {
    if (e.ts && e.ts > 0) { const d = new Date(e.ts * 1000); return d.toLocaleString('de-DE'); }
    return '+' + e.uptime + 's';
  }
  function fmtUnix(t) { return t ? new Date(t * 1000).toLocaleString('de-DE', { dateStyle: 'short', timeStyle: 'short' }) : ''; }
  function fmtUptime(s) {
    const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
    return (d ? d + 'd ' : '') + h + 'h ' + m + 'm';
  }
  function pad(n) { return String(n).padStart(2, '0'); }
  function unixToLocalInput(t) {
    if (!t) return '';
    const d = new Date(t * 1000);
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}T${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }
  function localInputToUnix(s) { return s ? Math.floor(new Date(s).getTime() / 1000) : 0; }
  function minToTime(m) { return pad(Math.floor(m / 60)) + ':' + pad(m % 60); }
  function timeToMin(s) { const p = (s || '0:0').split(':'); return (Number(p[0]) || 0) * 60 + (Number(p[1]) || 0); }

  function formData(form) {
    const o = {};
    $$('input,select', form).forEach(i => {
      if (!i.name) return;
      if (i.type === 'checkbox') o[i.name] = i.checked;
      else if (i.type === 'radio') { if (i.checked) o[i.name] = i.value; }
      else if (i.type === 'number') o[i.name] = i.value === '' ? null : Number(i.value);
      else o[i.name] = i.value;
    });
    return o;
  }
  function fillForm(form, data) {
    $$('input,select', form).forEach(i => {
      if (!i.name || !(i.name in data)) return;
      const v = data[i.name];
      if (i.type === 'checkbox') i.checked = !!v;
      else if (i.type === 'radio') i.checked = (i.value === String(v));
      else i.value = v == null ? '' : v;
    });
  }

  /* ---------- Routing ---------- */
  function show(id) {
    $$('.view').forEach(v => v.classList.add('hidden'));
    $('#view-' + id).classList.remove('hidden');
    $$('#nav a').forEach(a => a.classList.toggle('active', a.getAttribute('href') === '#' + id));
  }

  async function route() {
    clearInterval(refreshTimer);
    if (!session.authenticated) {
      try { session = await api('/api/session'); } catch (e) { toast('Gerät nicht erreichbar: ' + e.message, 'bad'); return; }
    }
    $('#hd-host').textContent = session.hostname ? session.hostname : 'Key System';
    $('#tls-warn').classList.toggle('hidden', !!session.tls);
    if (!session.authenticated) {
      $('#nav').classList.add('hidden');
      show(session.setup_required ? 'setup' : 'login');
      if (!session.setup_required) showLoginStep(!!twofaTicket);
      return;
    }
    $('#nav').classList.remove('hidden');
    const view = (location.hash || '#dashboard').slice(1);
    const loaders = { dashboard: loadDashboard, codes: loadCodes, log: loadLog, network: loadNetwork, cert: loadCert, firmware: loadFirmware, system: loadSystem };
    if (!loaders[view]) { location.hash = '#dashboard'; return; }
    show(view);
    try { await loaders[view](); } catch (e) { toast(e.message, 'bad'); }
    if (view === 'dashboard') refreshTimer = setInterval(() => loadDashboard().catch(() => {}), 5000);
    if (view === 'system') refreshTimer = setInterval(() => loadLearn().catch(() => {}), 2000);
  }

  /* ---------- Übersicht ---------- */
  async function loadDashboard() {
    const s = await api('/api/status');
    const d = s.door;
    kv($('#dash-door'), [
      ['Relais', d.relay_active ? 'aktiv (Tür offen)' : 'aus', d.relay_active ? 'warn' : ''],
      ['Türkontakt', d.contact_enabled ? (d.contact_closed ? 'geschlossen' : 'offen') : 'nicht konfiguriert', d.contact_enabled && !d.contact_closed ? 'warn' : ''],
      ['Keypad', d.keypad_locked ? `gesperrt (${d.keypad_lock_remaining_s} s)` : (d.keypad_fail_count ? `bereit, ${d.keypad_fail_count} Fehlversuch(e)` : 'bereit'), d.keypad_locked ? 'bad' : 'ok'],
      ['Keypad-Daten', d.keypad_rx_count ? `${d.keypad_rx_count} Byte, zuletzt vor ${fmtUptime(s.system.uptime_s - d.keypad_last_rx_uptime_s)}` : 'noch nichts empfangen', d.keypad_rx_count ? '' : 'warn'],
      ['Letzte Öffnung', d.last_open_uptime_s ? 'vor ' + fmtUptime(s.system.uptime_s - d.last_open_uptime_s) : 'seit Start keine'],
      ['Codes', d.codes_count]
    ]);
    kv($('#dash-net'), [
      ['Link', s.network.link ? `${s.network.speed} Mbit/s ${s.network.full_duplex ? 'Vollduplex' : 'Halbduplex'}` : 'kein Link', s.network.link ? 'ok' : 'bad'],
      ['Hostname', s.network.hostname + '.local'],
      ['IPv4', s.network.has_ip4 ? `${s.network.ip4} / ${s.network.mask4}` + (s.network.ip4_fallback ? ' (Rückfall, kein DHCP)' : s.network.ip4_mode === 'static' ? ' (statisch)' : ' (DHCP)') : 'keine', s.network.has_ip4 ? (s.network.ip4_fallback ? 'warn' : '') : 'warn'],
      ['Gateway', s.network.gw4],
      ['DNS', [s.network.dns1, s.network.dns2].filter(x => x && x !== '0.0.0.0').join(', ')],
      ['IPv6', s.network.ip6.join('<br>')],
      ['MAC', s.network.mac]
    ]);
    kv($('#dash-sys'), [
      ['Zeit', s.system.time + (s.system.time_valid ? '' : ' (nicht synchron)'), s.system.time_valid ? '' : 'warn'],
      ['Laufzeit', fmtUptime(s.system.uptime_s)],
      ['Firmware', `${s.firmware.version} (${s.firmware.partition})`],
      ['Heap frei', Math.round(s.system.heap_free / 1024) + ' KB (min ' + Math.round(s.system.heap_min / 1024) + ' KB)'],
      ['PSRAM frei', Math.round(s.system.psram_free / 1024) + ' KB'],
      ['Dateisystem', Math.round(s.system.fs_used / 1024) + ' / ' + Math.round(s.system.fs_total / 1024) + ' KB'],
      ['Verbindung', s.system.tls ? 'HTTPS' : 'HTTP (unverschlüsselt!)', s.system.tls ? 'ok' : 'bad']
    ]);
    const l = await api('/api/log?max=8');
    $('#dash-log tbody').innerHTML = l.entries.map(e =>
      `<tr><td class="t">${fmtTs(e)}</td><td><span class="cat ${e.cat}">${e.cat}</span></td><td>${esc(e.msg)}</td></tr>`).join('');
  }

  /* ---------- Codes ---------- */
  const TYPE_NAMES = { permanent: 'Dauercode', timed: 'Zeitcode', onetime: 'Einmalcode' };
  const WD = ['Mo', 'Di', 'Mi', 'Do', 'Fr', 'Sa', 'So'];
  let codesCache = [];

  function codeValidity(c) {
    const parts = [];
    if (c.valid_from) parts.push('ab ' + fmtUnix(c.valid_from));
    if (c.valid_until) parts.push('bis ' + fmtUnix(c.valid_until));
    if (c.win_enable) {
      const days = WD.filter((_, i) => c.weekdays & (1 << i));
      parts.push((days.length === 7 ? 'täglich' : days.join(',')) + ' ' + minToTime(c.win_from) + '–' + minToTime(c.win_to));
    }
    return parts.length ? parts.join('<br>') : 'unbegrenzt';
  }
  function codeStatus(c, now) {
    if (!c.enabled) return ['deaktiviert', 'warn'];
    if (c.max_uses && c.uses >= c.max_uses) return ['aufgebraucht', 'bad'];
    if (c.valid_until && now > c.valid_until) return ['abgelaufen', 'bad'];
    if (c.valid_from && now < c.valid_from) return ['noch nicht gültig', 'warn'];
    return ['aktiv', 'ok'];
  }
  async function loadCodes() {
    const r = await api('/api/codes');
    codesCache = r.codes;
    $('#codes-count').textContent = `(${r.codes.length} / ${r.max})`;
    $('#codes-time-warn').classList.toggle('hidden', r.time_valid);
    const now = Math.floor(Date.now() / 1000);
    $('#codes-table tbody').innerHTML = r.codes.map(c => {
      const st = codeStatus(c, now);
      return `<tr data-id="${c.id}">
        <td>${esc(c.name)}</td><td><code>${esc(c.code)}</code></td><td>${TYPE_NAMES[c.type] || c.type}</td>
        <td>${codeValidity(c)}</td><td>${c.uses}${c.max_uses ? ' / ' + c.max_uses : ''}</td>
        <td class="t">${fmtUnix(c.last_used) || '–'}</td><td><span class="badge ${st[1]}">${st[0]}</span></td>
        <td class="actions"><button class="secondary" data-act="edit">Bearbeiten</button> <button class="danger" data-act="del">Löschen</button></td>
      </tr>`;
    }).join('') || '<tr><td colspan="8" class="hint">Noch keine Codes angelegt.</td></tr>';
  }
  function openCodeEditor(c) {
    const f = $('#form-code');
    f.reset();
    $('#code-editor-title').textContent = c ? `Code bearbeiten: ${c.name}` : 'Neuer Code';
    const data = c ? Object.assign({}, c) : { id: '', type: 'permanent', enabled: true, weekdays: 0x7F, win_from: 360, win_to: 1200, max_uses: 0 };
    data.valid_from_local = unixToLocalInput(data.valid_from);
    data.valid_until_local = unixToLocalInput(data.valid_until);
    data.win_from_t = minToTime(data.win_from || 0);
    data.win_to_t = minToTime(data.win_to || 0);
    fillForm(f, data);
    $$('#code-weekdays input').forEach(i => i.checked = !!(data.weekdays & (1 << Number(i.dataset.wd))));
    toggleCodeFields();
    $('#code-editor').classList.remove('hidden');
    $('#code-editor').scrollIntoView({ behavior: 'smooth' });
  }
  function toggleCodeFields() {
    const f = $('#form-code');
    $('#code-window').classList.toggle('hidden', !f.win_enable.checked);
    $('#code-maxuses-row').classList.toggle('hidden', f.type.value === 'onetime');
  }
  $('#btn-code-new').addEventListener('click', () => openCodeEditor(null));
  $('#btn-code-cancel').addEventListener('click', () => $('#code-editor').classList.add('hidden'));
  $('#form-code').addEventListener('change', toggleCodeFields);
  $('#codes-table').addEventListener('click', async (ev) => {
    const btn = ev.target.closest('button[data-act]');
    if (!btn) return;
    const id = Number(btn.closest('tr').dataset.id);
    const c = codesCache.find(x => x.id === id);
    if (btn.dataset.act === 'edit') return openCodeEditor(c);
    if (!confirm(`Code "${c.name}" löschen?`)) return;
    try { await api('/api/codes/' + id, { method: 'DELETE' }); toast('Gelöscht', 'ok'); await loadCodes(); }
    catch (e) { toast(e.message, 'bad'); }
  });
  onSubmit('#form-code', async (f) => {
    const d = formData(f);
    let weekdays = 0;
    $$('#code-weekdays input').forEach(i => { if (i.checked) weekdays |= 1 << Number(i.dataset.wd); });
    const body = {
      name: d.name, code: d.code, type: d.type, enabled: d.enabled,
      valid_from: localInputToUnix(d.valid_from_local), valid_until: localInputToUnix(d.valid_until_local),
      win_enable: d.win_enable, weekdays, win_from: timeToMin(d.win_from_t), win_to: timeToMin(d.win_to_t),
      max_uses: d.type === 'onetime' ? 1 : (d.max_uses || 0)
    };
    if (body.type === 'timed' && !body.valid_from && !body.valid_until) throw new Error('Zeitcode braucht "gültig ab" oder "gültig bis"');
    if (body.valid_from && body.valid_until && body.valid_from >= body.valid_until) throw new Error('"Gültig bis" muss nach "gültig ab" liegen');
    if (d.id) await api('/api/codes/' + d.id, { method: 'PUT', json: body });
    else await api('/api/codes', { method: 'POST', json: body });
    toast('Gespeichert', 'ok');
    $('#code-editor').classList.add('hidden');
    await loadCodes();
  });

  /* ---------- Protokoll ---------- */
  let logCache = [];
  async function loadLog() {
    const l = await api('/api/log?max=200');
    logCache = l.entries;
    renderLog();
  }
  function renderLog() {
    const f = $('#log-filter').value;
    $('#log-table tbody').innerHTML = logCache.filter(e => !f || e.cat === f).map(e =>
      `<tr><td class="t">${fmtTs(e)}</td><td><span class="cat ${e.cat}">${e.cat}</span></td><td>${esc(e.msg)}</td></tr>`).join('');
  }

  /* ---------- Netzwerk ---------- */
  async function loadNetwork() {
    const n = await api('/api/network');
    fillForm($('#form-network'), n);
    toggleNetFields();
  }
  function toggleNetFields() {
    const f = $('#form-network');
    const st = f.ipv4_mode.value === 'static';
    $('#ipv4-static').classList.toggle('hidden', !st);
    $('#ipv6-static').classList.toggle('hidden', !f.ipv6_static_enable.checked);
    $('#net-mdns').textContent = f.hostname.value || 'hostname';
  }

  /* ---------- Zertifikat ---------- */
  async function loadCert() {
    const c = await api('/api/cert');
    kv($('#cert-info'), [
      ['Status', c.available ? (c.uploaded ? 'eigenes Zertifikat' : 'gerätespezifisch, selbstsigniert') : 'kein Zertifikat (HTTP-only)', c.available ? 'ok' : 'bad'],
      ['Verbindung', c.tls_active ? 'HTTPS aktiv' : 'HTTP', c.tls_active ? 'ok' : 'bad'],
      ['Subject', esc(c.subject || '')],
      ['Aussteller', esc(c.issuer || '')],
      ['Gültig', c.valid_from ? `${c.valid_from} bis ${c.valid_to}` : ''],
      ['Schlüssel', c.key_type],
      ['Key passt', c.available ? (c.key_matches ? 'ja' : 'NEIN') : '', c.key_matches ? 'ok' : 'bad'],
      ['SHA-256', c.fingerprint_sha256]
    ]);
    const stat = (ok) => ok ? '<span class="ok">✓ vorhanden</span>' : '<span class="warn">⚠ nicht hochgeladen (wird beim Boot erzeugt)</span>';
    $('#cert-file-status').innerHTML = stat(c.cert_file);
    $('#key-file-status').innerHTML = stat(c.key_file);
  }

  /* ---------- Firmware ---------- */
  async function loadFirmware() {
    const f = await api('/api/firmware');
    const r = f.running || {}, n = f.next || {};
    kv($('#fw-info'), [
      ['Version', r.version], ['Projekt', r.project], ['ESP-IDF', r.idf], ['Gebaut', r.built],
      ['Partition', r.label ? `${r.label} (${Math.round(r.size / 1024)} KB, Status ${r.state || '?'})` : ''],
      ['Signaturpflicht', f.signature_required ? 'ja (RSA-3072)' : 'nein', 'ok']
    ]);
    $('#fw-pending').classList.toggle('hidden', !f.pending_verify);
    kv($('#fw-other'), [
      ['Partition', n.label ? `${n.label} (Status ${n.state || 'leer'})` : ''],
      ['Version', n.version || 'keine Firmware vorhanden'], ['Gebaut', n.built]
    ]);
    $('#btn-fw-rollback').disabled = !n.version;
    if (f.state === 4 && f.last_error) $('#fw-status').textContent = 'Letzter Fehler: ' + f.last_error;
  }
  function uploadFirmware(file) {
    return new Promise((resolve, reject) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/firmware');
      xhr.setRequestHeader('X-CSRF-Token', session.csrf || '');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = (ev) => {
        if (ev.lengthComputable) {
          $('#fw-progress').value = Math.round(ev.loaded / ev.total * 100);
          $('#fw-status').textContent = `Übertrage … ${Math.round(ev.loaded / 1024)} / ${Math.round(ev.total / 1024)} KB`;
        }
      };
      xhr.onload = () => {
        let d = null; try { d = JSON.parse(xhr.responseText); } catch (e) { }
        if (xhr.status >= 200 && xhr.status < 300) resolve(d);
        else reject(new Error((d && d.error) || ('HTTP ' + xhr.status)));
      };
      xhr.onerror = () => reject(new Error('Verbindung abgebrochen'));
      xhr.send(file);
    });
  }
  $('#btn-fw-upload').addEventListener('click', async () => {
    const file = $('#file-fw').files[0];
    if (!file) return toast('Bitte zuerst firmware.bin wählen', 'bad');
    if (!confirm(`Firmware "${file.name}" (${Math.round(file.size / 1024)} KB) hochladen?`)) return;
    const btn = $('#btn-fw-upload');
    btn.disabled = true;
    $('#fw-progress').classList.remove('hidden'); $('#fw-progress').value = 0;
    try {
      await uploadFirmware(file);
      $('#fw-status').textContent = 'Signatur geprüft, Image installiert. Neustart …';
      toast('Update installiert, Gerät startet neu', 'ok');
      await api('/api/system/reboot', { method: 'POST' });
      setTimeout(() => location.reload(), 25000);
    } catch (e) {
      $('#fw-status').textContent = 'Fehler: ' + e.message;
      toast(e.message, 'bad');
      btn.disabled = false;
    }
  });
  $('#btn-fw-rollback').addEventListener('click', async () => {
    if (!confirm('Beim nächsten Start die vorherige Firmware booten und jetzt neu starten?')) return;
    try { await api('/api/firmware/rollback', { method: 'POST' }); await api('/api/system/reboot', { method: 'POST' }); toast('Neustart …', 'ok'); setTimeout(() => location.reload(), 25000); }
    catch (e) { toast(e.message, 'bad'); }
  });

  /* ---------- System ---------- */
  async function loadSystem() {
    const s = await api('/api/system');
    fillForm($('#form-door'), s.door);
    fillForm($('#form-keypad'), s.keypad);
    $('#form-password').user.value = s.admin_user;
    $('#learn-panel').classList.toggle('hidden', !s.keypad.learn_mode);
    if (s.keypad.learn_mode) await loadLearn();
    await loadTwofa();
  }
  async function loadLearn() {
    if ($('#learn-panel').classList.contains('hidden')) return;
    const r = await api('/api/keypad/raw');
    kv($('#learn-info'), [
      ['Empfangen gesamt', r.rx_total + ' Byte'],
      ['Zuletzt', r.last_rx_uptime_s ? 'vor ' + (r.uptime_s - r.last_rx_uptime_s) + ' s' : 'noch nichts', r.rx_total ? '' : 'warn']
    ]);
    $('#learn-hex').textContent = r.hex || '(leer)';
    $('#learn-ascii').textContent = r.ascii || '';
  }

  /* ---------- Events ---------- */
  function onSubmit(id, fn) {
    $(id).addEventListener('submit', async (ev) => {
      ev.preventDefault();
      const btn = ev.target.querySelector('button[type=submit]');
      btn.disabled = true;
      try { await fn(ev.target); } catch (e) { toast(e.message, 'bad'); }
      btn.disabled = false;
    });
  }

  onSubmit('#form-setup', async (f) => {
    const d = formData(f);
    if (d.password !== d.password2) throw new Error('Passwörter stimmen nicht überein');
    const r = await api('/api/setup', { method: 'POST', json: { user: d.user, password: d.password } });
    session = { authenticated: true, csrf: r.csrf, user: r.user, hostname: session.hostname, tls: session.tls };
    toast('Eingerichtet und angemeldet', 'ok'); location.hash = '#dashboard'; route();
  });
  let twofaTicket = null;
  function showLoginStep(step2) {
    $('#form-login').classList.toggle('hidden', step2);
    $('#form-login-2fa').classList.toggle('hidden', !step2);
    if (step2) $('#form-login-2fa').code.focus();
  }
  onSubmit('#form-login', async (f) => {
    const d = formData(f);
    const r = await api('/api/login', { method: 'POST', json: { user: d.user, password: d.password } });
    if (r.twofa_required) {
      twofaTicket = r.ticket;
      $('#twofa-time-warn').classList.toggle('hidden', !!r.time_valid);
      $('#twofa-rec-left').textContent = `${r.recovery_remaining} Recovery-Code(s) verfügbar.`;
      f.password.value = '';
      showLoginStep(true);
      return;
    }
    session = { authenticated: true, csrf: r.csrf, user: r.user, hostname: session.hostname, tls: session.tls };
    f.reset(); route();
  });
  onSubmit('#form-login-2fa', async (f) => {
    const r = await api('/api/login/2fa', { method: 'POST', json: { ticket: twofaTicket, code: f.code.value.trim() } });
    session = { authenticated: true, csrf: r.csrf, user: r.user, hostname: session.hostname, tls: session.tls };
    f.reset(); twofaTicket = null; showLoginStep(false); route();
  });
  $('#btn-2fa-back').addEventListener('click', () => { twofaTicket = null; $('#form-login-2fa').reset(); showLoginStep(false); });

  /* ---------- 2FA-Verwaltung (System-Seite) ---------- */
  async function loadTwofa() {
    const t = await api('/api/twofa');
    kv($('#twofa-info'), [
      ['Status', t.enabled ? 'aktiv' : 'nicht aktiv', t.enabled ? 'ok' : 'warn'],
      ['Recovery-Codes', t.enabled ? `${t.recovery_remaining} von ${t.recovery_total} übrig` : '', t.enabled && t.recovery_remaining <= 2 ? 'warn' : ''],
      ['Uhrzeit', t.time_valid ? 'synchron (NTP)' : 'nicht synchron', t.time_valid ? 'ok' : 'bad']
    ]);
    $('#twofa-sys-time-warn').classList.toggle('hidden', t.time_valid);
    $('#twofa-off').classList.toggle('hidden', t.enabled);
    $('#twofa-on').classList.toggle('hidden', !t.enabled);
    $('#twofa-setup').classList.add('hidden');
    $('#twofa-recovery-new').classList.add('hidden');
  }
  function renderQr(text) {
    const el = $('#twofa-qr');
    try {
      const q = qrcode(0, 'M'); q.addData(text); q.make();
      el.innerHTML = q.createSvgTag({ cellSize: 4, margin: 0, scalable: true });
    } catch (e) { el.textContent = 'QR konnte nicht erzeugt werden: ' + e.message; }
  }
  let recoveryText = '';
  onSubmit('#form-twofa-setup', async (f) => {
    const r = await api('/api/twofa/setup', { method: 'POST', json: { password: f.password.value } });
    f.reset();
    renderQr(r.otpauth);
    $('#twofa-secret').textContent = r.secret.replace(/(.{4})/g, '$1 ').trim();
    recoveryText = r.recovery_codes.join('\n');
    $('#twofa-recovery').textContent = recoveryText;
    $('#twofa-off').classList.add('hidden');
    $('#twofa-setup').classList.remove('hidden');
    toast(`Einrichtung gestartet, bitte innerhalb von ${Math.round(r.expires_s / 60)} Minuten bestätigen`, 'ok');
  });
  onSubmit('#form-twofa-confirm', async (f) => {
    await api('/api/twofa/confirm', { method: 'POST', json: { code: f.code.value.trim() } });
    toast('2FA ist aktiv. Beim nächsten Login wird der App-Code verlangt.', 'ok');
    f.reset(); await loadTwofa();
  });
  $('#btn-twofa-cancel').addEventListener('click', async () => {
    try { await api('/api/twofa/cancel', { method: 'POST' }); } catch (e) { }
    await loadTwofa();
  });
  $('#btn-twofa-copy').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(recoveryText); toast('Recovery-Codes kopiert', 'ok'); }
    catch (e) { toast('Kopieren nicht möglich, bitte markieren und kopieren', 'bad'); }
  });
  $('#btn-twofa-print').addEventListener('click', () => window.print());
  $('#form-twofa-manage').addEventListener('submit', async (ev) => {
    ev.preventDefault();
    const act = ev.submitter && ev.submitter.dataset.act;
    const f = ev.target;
    const body = { password: f.password.value, code: f.code.value.trim() };
    try {
      if (act === 'disable') {
        if (!confirm('Zwei-Faktor-Authentifizierung wirklich deaktivieren?')) return;
        await api('/api/twofa/disable', { method: 'POST', json: body });
        toast('2FA deaktiviert', 'ok'); f.reset(); await loadTwofa();
      } else {
        if (!confirm('Neue Recovery-Codes erzeugen? Die bisherigen werden ungültig.')) return;
        const r = await api('/api/twofa/recovery', { method: 'POST', json: body });
        f.reset(); await loadTwofa();
        recoveryText = r.recovery_codes.join('\n');
        const pre = $('#twofa-recovery-new');
        pre.textContent = 'Neue Recovery-Codes (nur jetzt sichtbar):\n' + recoveryText;
        pre.classList.remove('hidden');
        toast('Neue Recovery-Codes erzeugt, bitte sichern', 'ok');
      }
    } catch (e) { toast(e.message, 'bad'); }
  });
  $('#btn-logout').addEventListener('click', async (ev) => {
    ev.preventDefault();
    try { await api('/api/logout', { method: 'POST' }); } catch (e) { }
    session = { authenticated: false }; route();
  });
  $('#btn-open').addEventListener('click', async () => {
    try { await api('/api/door/open', { method: 'POST' }); toast('Tür geöffnet', 'ok'); loadDashboard().catch(() => {}); }
    catch (e) { toast(e.message, 'bad'); }
  });
  $('#log-filter').addEventListener('change', renderLog);
  $('#btn-log-refresh').addEventListener('click', () => loadLog().catch(e => toast(e.message, 'bad')));
  $('#btn-log-csv').addEventListener('click', () => {
    const rows = [['id', 'zeit', 'uptime_s', 'kategorie', 'meldung']].concat(logCache.map(e =>
      [e.id, e.ts ? new Date(e.ts * 1000).toISOString() : '', e.uptime, e.cat, e.msg]));
    const csv = rows.map(r => r.map(v => '"' + String(v).replace(/"/g, '""') + '"').join(';')).join('\r\n');
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob(['﻿' + csv], { type: 'text/csv' }));
    a.download = 'keysystem-protokoll.csv'; a.click();
  });

  $('#form-network').addEventListener('change', toggleNetFields);
  $('#form-network').hostname.addEventListener('input', toggleNetFields);
  onSubmit('#form-network', async (f) => {
    const r = await api('/api/network', { method: 'POST', json: formData(f) });
    toast(r.reboot_required ? 'Gespeichert. Aktiv nach Neustart (System → Neustart).' : 'Gespeichert', 'ok');
  });

  async function uploadPem(fileInput, type) {
    const file = fileInput.files[0];
    if (!file) throw new Error('Bitte zuerst eine Datei wählen');
    const text = await file.text();
    const r = await api('/api/cert/upload?type=' + type, { method: 'POST', headers: { 'Content-Type': 'text/plain' }, body: text });
    toast('Hochgeladen. ' + (r.reboot_required ? 'Aktiv nach Neustart.' : ''), 'ok');
    await loadCert();
  }
  $('#btn-upload-cert').addEventListener('click', () => uploadPem($('#file-cert'), 'cert').catch(e => toast(e.message, 'bad')));
  $('#btn-upload-key').addEventListener('click', () => uploadPem($('#file-key'), 'key').catch(e => toast(e.message, 'bad')));
  $('#btn-cert-regen').addEventListener('click', async () => {
    if (!confirm('Zertifikat und Key löschen und beim Neustart neu erzeugen?')) return;
    try { await api('/api/cert/regenerate', { method: 'POST' }); await api('/api/system/reboot', { method: 'POST' }); toast('Neustart …', 'ok'); }
    catch (e) { toast(e.message, 'bad'); }
  });

  onSubmit('#form-door', async (f) => { await api('/api/system', { method: 'POST', json: { door: formData(f) } }); toast('Gespeichert', 'ok'); });
  onSubmit('#form-keypad', async (f) => {
    const k = formData(f); k.baud = Number(k.baud); k.parity = Number(k.parity); k.frame_mode = Number(k.frame_mode);
    await api('/api/system', { method: 'POST', json: { keypad: k } }); toast('Gespeichert', 'ok');
    await loadSystem();
  });
  onSubmit('#form-password', async (f) => {
    const d = formData(f);
    await api('/api/system/password', { method: 'POST', json: d });
    toast('Passwort geändert, bitte neu anmelden', 'ok');
    session = { authenticated: false }; route();
  });
  $('#btn-restore').addEventListener('click', async () => {
    const file = $('#file-restore').files[0];
    if (!file) return toast('Bitte zuerst eine Backup-Datei wählen', 'bad');
    let data;
    try { data = JSON.parse(await file.text()); } catch (e) { return toast('Datei ist kein gültiges JSON', 'bad'); }
    if (!confirm('Backup einspielen? Vorhandene Codes werden ersetzt.')) return;
    data.restore_network = $('#restore-net').checked;
    try { const r = await api('/api/restore', { method: 'POST', json: data }); toast('Eingespielt.' + (r.reboot_required ? ' Netzwerkänderungen nach Neustart aktiv.' : ''), 'ok'); }
    catch (e) { toast(e.message, 'bad'); }
  });
  $('#btn-reboot').addEventListener('click', async () => {
    if (!confirm('Gerät neu starten?')) return;
    try { await api('/api/system/reboot', { method: 'POST' }); toast('Neustart … Seite in 20 s neu laden.', 'ok'); setTimeout(() => location.reload(), 20000); }
    catch (e) { toast(e.message, 'bad'); }
  });
  $('#btn-factory').addEventListener('click', async () => {
    if (!confirm('WERKSRESET: Alle Einstellungen, Codes, Zertifikat und Passwort löschen?')) return;
    if (!confirm('Wirklich? Das Gerät startet danach mit DHCP und Ersteinrichtung.')) return;
    try { await api('/api/system/factory-reset', { method: 'POST' }); toast('Werksreset, Gerät startet neu …', 'ok'); }
    catch (e) { toast(e.message, 'bad'); }
  });

  window.addEventListener('hashchange', route);
  route();
})();
