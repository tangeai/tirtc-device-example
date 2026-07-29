const deviceAddress = document.getElementById('device-address');
const deviceDot = document.getElementById('device-dot');
const deviceState = document.getElementById('device-state');
const deviceResolution = document.getElementById('device-resolution');
const clock = document.getElementById('clock');
const screen = document.getElementById('device-screen');
const screenError = document.getElementById('screen-error');
const screenToggle = document.getElementById('screen-toggle');
const logOutput = document.getElementById('log-output');
const logFile = document.getElementById('log-file');
const logRefresh = document.getElementById('log-refresh');
const autoScroll = document.getElementById('auto-scroll');
const lastUpdate = document.getElementById('last-update');

let screenPaused = false;
let frameTimer = null;

function setDeviceOnline(online) {
  deviceDot.classList.toggle('online', online);
  deviceState.textContent = online ? 'Online' : 'Offline';
}

function scheduleFrame(delay) {
  clearTimeout(frameTimer);
  if (!screenPaused) frameTimer = setTimeout(loadFrame, delay);
}

function loadFrame() {
  screen.src = `/api/device/screen.bmp?t=${Date.now()}`;
}

screen.addEventListener('load', () => {
  screenError.hidden = true;
  setDeviceOnline(true);
  scheduleFrame(900);
});

screen.addEventListener('error', () => {
  screenError.hidden = false;
  screenError.textContent = 'Frame unavailable';
  setDeviceOnline(false);
  scheduleFrame(1800);
});

screenToggle.addEventListener('click', () => {
  screenPaused = !screenPaused;
  screenToggle.textContent = screenPaused ? 'Resume' : 'Pause';
  if (screenPaused) {
    clearTimeout(frameTimer);
  } else {
    loadFrame();
  }
});

async function refreshStatus() {
  try {
    const response = await fetch('/api/device/status', { cache: 'no-store' });
    const status = await response.json();
    if (!response.ok || !status.ok) throw new Error(status.error || 'Device offline');
    deviceAddress.textContent = `${status.chip}  ${status.ip}:${status.port}`;
    deviceResolution.textContent = `${status.width} x ${status.height}`;
    setDeviceOnline(true);
  } catch (error) {
    deviceAddress.textContent = 'Waiting for device';
    setDeviceOnline(false);
  }
}

async function refreshLogs() {
  try {
    const response = await fetch('/api/logs?tail=700', { cache: 'no-store' });
    const payload = await response.json();
    if (!response.ok || !payload.ok) throw new Error(payload.error || 'Log unavailable');
    logOutput.textContent = payload.lines.join('\n');
    logFile.textContent = payload.file || 'No log file';
    lastUpdate.textContent = payload.updated_at ? `Log ${payload.updated_at}` : 'No log data';
    if (autoScroll.checked) logOutput.scrollTop = logOutput.scrollHeight;
  } catch (error) {
    lastUpdate.textContent = 'Log refresh failed';
  }
}

logRefresh.addEventListener('click', refreshLogs);

setInterval(() => {
  clock.textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false });
}, 1000);
setInterval(refreshStatus, 2500);
setInterval(refreshLogs, 1200);

clock.textContent = new Date().toLocaleTimeString('zh-CN', { hour12: false });
refreshStatus();
refreshLogs();
loadFrame();
