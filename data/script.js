const state = {
  busy: false,
  counterEnabled: true,
  diskEnabled: false,
  counterRPM: 16,
  diskRPM: 5,
  stepsPerDigit: 6600,
  stepsPerRevolution: 2048,
  selectedStep: 1,
  subscribers: -1,
  youtubeLine: '',
  statusRaw: '',
  lastDigits: 0,
  lastSteps: 0,
  pollError: null,
};

const els = {
  counterIconBtn: document.getElementById('counterIconBtn'),
  diskIconBtn: document.getElementById('diskIconBtn'),
  counterRpmBtn: document.getElementById('counterRpmBtn'),
  diskRpmBtn: document.getElementById('diskRpmBtn'),
  stepsPerDigitBtn: document.getElementById('stepsPerDigitBtn'),
  statusText: document.getElementById('statusText'),
  stepsRow: document.getElementById('stepsRow'),
  stepHighlight: document.getElementById('stepHighlight'),
  stepOptions: Array.from(document.querySelectorAll('.step-option')),
  minusBtn: document.getElementById('minusBtn'),
  plusBtn: document.getElementById('plusBtn'),
  sheet: document.getElementById('sheet'),
  sheetBackdrop: document.getElementById('sheetBackdrop'),
  sheetTitle: document.getElementById('sheetTitle'),
  sheetInput: document.getElementById('sheetInput'),
  cancelSheetBtn: document.getElementById('cancelSheetBtn'),
  confirmSheetBtn: document.getElementById('confirmSheetBtn')
};

let currentEditKey = null;

function buildSettingsPayload() {
  return {
    stepsPerRevolution: state.stepsPerRevolution,
    motorCounterSpeedRPM: state.counterRPM,
    motorDiskSpeedRPM: state.diskRPM,
    stepsPerDigit: state.stepsPerDigit,
    counter: state.counterEnabled,
    disk: state.diskEnabled,
  };
}

function formatYoutubeLine(y) {
  if (!y) return 'YouTube: —';
  const s = String(y).trim();
  return s.startsWith('YouTube') ? s : `YouTube: ${s}`;
}

function statusFirstLine(raw) {
  if (!raw) return '—';
  const lines = String(raw).split(/\r?\n/).map((l) => l.trim()).filter(Boolean);
  return lines[0] || '—';
}

function fmtNum(n) {
  if (n === null || n === undefined || Number.isNaN(n)) return '—';
  if (typeof n === 'number') {
    if (Number.isInteger(n)) return String(n);
    const r = Math.round(n * 1000) / 1000;
    return String(r);
  }
  return String(n);
}

function applyStateFromApi(j) {
  state.busy = !!j.busy;
  state.counterEnabled = !!j.counterEnabled;
  state.diskEnabled = !!j.diskEnabled;
  state.counterRPM = Number(j.motorCounterSpeedRPM) || 0;
  state.diskRPM = Number(j.motorDiskSpeedRPM) || 0;
  state.stepsPerDigit = Number(j.stepsPerDigit) || 0;
  state.stepsPerRevolution = Number(j.stepsPerRevolution) || 2048;
  state.subscribers = typeof j.subscribers === 'number' ? j.subscribers : -1;
  state.youtubeLine = j.youtubeLine != null ? String(j.youtubeLine) : '';
  state.statusRaw = j.statusLine != null ? String(j.statusLine) : '';
  state.lastDigits = typeof j.lastDigits === 'number' ? j.lastDigits : 0;
  state.lastSteps = typeof j.lastSteps === 'number' ? j.lastSteps : 0;
  state.pollError = null;
}

async function refreshState() {
  try {
    const r = await fetch('/api/state', { cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    applyStateFromApi(j);
    render();
  } catch (e) {
    state.pollError = String(e.message || e);
    state.statusRaw = 'Нет связи с ESP32. Откройте страницу по IP устройства; в LittleFS должны быть index.html, style.css и script.js.';
    render();
  }
}

async function postSettings() {
  const r = await fetch('/settings', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(buildSettingsPayload()),
  });
  if (!r.ok) throw new Error(await r.text());
  await refreshState();
}

function setBusyUi(disabled) {
  els.minusBtn.disabled = disabled;
  els.plusBtn.disabled = disabled;
  els.stepOptions.forEach((b) => { b.disabled = disabled; });
  els.counterIconBtn.disabled = disabled;
  els.diskIconBtn.disabled = disabled;
}

function render() {
  els.counterIconBtn.classList.toggle('inactive', !state.counterEnabled);
  els.diskIconBtn.classList.toggle('inactive', !state.diskEnabled);

  els.counterRpmBtn.textContent = String(state.counterRPM);
  els.diskRpmBtn.textContent = String(state.diskRPM);
  els.stepsPerDigitBtn.textContent = String(state.stepsPerDigit);

  const sub = state.subscribers >= 0 ? state.subscribers : 'неизвестно';
  const yLine = formatYoutubeLine(state.youtubeLine);
  const st = statusFirstLine(state.statusRaw);
  const err = state.pollError ? `\nОшибка опроса: ${state.pollError}` : '';

  els.statusText.textContent =
`Подписчики: ${sub}
${yLine}
Статус: ${st}
Цифр: ${fmtNum(state.lastDigits)}
Сделано шагов: ${fmtNum(state.lastSteps)}${err}`;

  els.stepOptions.forEach((btn) => {
    const active = Number(btn.dataset.step) === state.selectedStep;
    btn.classList.toggle('active', active);
  });

  setBusyUi(state.busy);
  requestAnimationFrame(positionHighlight);
}

function positionHighlight() {
  const active = els.stepOptions.find(btn => Number(btn.dataset.step) === state.selectedStep);
  if (!active) return;
  const rowRect = els.stepsRow.getBoundingClientRect();
  const activeRect = active.getBoundingClientRect();
  const center = (activeRect.left - rowRect.left) + activeRect.width / 2;
  els.stepHighlight.style.left = `${center}px`;
}

function openSheet(title, key, value) {
  currentEditKey = key;
  els.sheetTitle.textContent = title;
  els.sheetInput.value = value;
  els.sheet.classList.add('open');
  els.sheetBackdrop.classList.add('open');
  setTimeout(() => {
    els.sheetInput.focus();
    els.sheetInput.select();
  }, 50);
}

function closeSheet() {
  currentEditKey = null;
  els.sheet.classList.remove('open');
  els.sheetBackdrop.classList.remove('open');
}

async function applySheetValue() {
  const value = parseInt(els.sheetInput.value, 10);
  if (!Number.isFinite(value) || value <= 0 || !currentEditKey) {
    closeSheet();
    return;
  }
  if (currentEditKey === 'counterRPM') state.counterRPM = value;
  else if (currentEditKey === 'diskRPM') state.diskRPM = value;
  else if (currentEditKey === 'stepsPerDigit') state.stepsPerDigit = value;
  closeSheet();
  try {
    await postSettings();
  } catch (e) {
    state.pollError = String(e.message || e);
    render();
  }
}

function flashButton(btn) {
  btn.classList.add('flash');
  setTimeout(() => btn.classList.remove('flash'), 160);
}

async function moveCounter(direction) {
  if (state.busy) return;
  const step = state.selectedStep;
  const digitCount = direction * step;
  try {
    const r = await fetch('/digitMove', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        ...buildSettingsPayload(),
        digitCount,
      }),
    });
    if (!r.ok) {
      const t = await r.text();
      throw new Error(t || ('HTTP ' + r.status));
    }
    await refreshState();
  } catch (e) {
    state.pollError = String(e.message || e);
    render();
  }
}

els.counterIconBtn.addEventListener('click', async () => {
  if (state.busy) return;
  state.counterEnabled = !state.counterEnabled;
  render();
  try {
    await postSettings();
  } catch (e) {
    state.counterEnabled = !state.counterEnabled;
    state.pollError = String(e.message || e);
    render();
  }
});

els.diskIconBtn.addEventListener('click', async () => {
  if (state.busy) return;
  state.diskEnabled = !state.diskEnabled;
  render();
  try {
    await postSettings();
  } catch (e) {
    state.diskEnabled = !state.diskEnabled;
    state.pollError = String(e.message || e);
    render();
  }
});

els.counterRpmBtn.addEventListener('click', () => openSheet('Скорость счетчика (RPM)', 'counterRPM', state.counterRPM));
els.diskRpmBtn.addEventListener('click', () => openSheet('Скорость диска (RPM)', 'diskRPM', state.diskRPM));
els.stepsPerDigitBtn.addEventListener('click', () => openSheet('Шагов на 1 цифру', 'stepsPerDigit', state.stepsPerDigit));

els.stepOptions.forEach(btn => {
  btn.addEventListener('click', () => {
    if (state.busy) return;
    state.selectedStep = Number(btn.dataset.step);
    render();
  });
});

els.minusBtn.addEventListener('click', () => {
  flashButton(els.minusBtn);
  moveCounter(-1);
});

els.plusBtn.addEventListener('click', () => {
  flashButton(els.plusBtn);
  moveCounter(1);
});

els.sheetBackdrop.addEventListener('click', closeSheet);
els.cancelSheetBtn.addEventListener('click', closeSheet);
els.confirmSheetBtn.addEventListener('click', () => { applySheetValue(); });
els.sheetInput.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') applySheetValue();
  if (e.key === 'Escape') closeSheet();
});

window.addEventListener('resize', positionHighlight);

refreshState();
setInterval(refreshState, 1000);
