#pragma once
#include <Arduino.h>

// Self-hosted control dashboard, served from the ESP32 (no external host needed).
// Polls /api/state and posts changes to /api/relay and /api/ac.
static const char DASHBOARD_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Smart Home</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { font-family: system-ui, sans-serif; margin: 0; background: #0f1115; color: #e8eaed; }
  header { padding: 18px 20px; font-size: 20px; font-weight: 600; border-bottom: 1px solid #232733; }
  main { max-width: 560px; margin: 0 auto; padding: 16px; }
  .card { background: #171a21; border: 1px solid #232733; border-radius: 14px; padding: 16px; margin: 12px 0; }
  .row { display: flex; align-items: center; justify-content: space-between; padding: 10px 0; }
  .name { font-size: 16px; }
  .switch { position: relative; width: 52px; height: 30px; }
  .switch input { display: none; }
  .slot { position: absolute; inset: 0; background: #3a3f4b; border-radius: 999px; transition: .2s; }
  .slot::before { content: ""; position: absolute; width: 24px; height: 24px; left: 3px; top: 3px; background: #fff; border-radius: 50%; transition: .2s; }
  input:checked + .slot { background: #2e7d32; }
  input:checked + .slot::before { transform: translateX(22px); }
  h2 { font-size: 14px; text-transform: uppercase; letter-spacing: .05em; color: #9aa0ac; margin: 4px 0 8px; }
  .ac-temp { font-size: 40px; font-weight: 700; text-align: center; margin: 8px 0; }
  .ctrls { display: flex; gap: 10px; justify-content: center; align-items: center; }
  button { background: #232733; color: #e8eaed; border: none; border-radius: 10px; padding: 12px 18px; font-size: 18px; cursor: pointer; }
  button:active { background: #2e323d; }
  select { background: #232733; color: #e8eaed; border: 1px solid #333; border-radius: 10px; padding: 10px; font-size: 15px; }
  .muted { color: #9aa0ac; font-size: 12px; text-align: center; margin-top: 18px; }
</style>
</head>
<body>
<header>🏠 Smart Home</header>
<main>
  <div class="card">
    <h2>Lights &amp; Fan</h2>
    <div id="relays"></div>
  </div>
  <div class="card">
    <h2>Air Conditioner</h2>
    <div class="row"><span class="name">Power</span>
      <label class="switch"><input type="checkbox" id="acPower"><span class="slot"></span></label>
    </div>
    <div class="ac-temp"><span id="acTemp">24</span>&deg;C</div>
    <div class="ctrls">
      <button onclick="bumpTemp(-1)">&minus;</button>
      <select id="acMode" onchange="sendAc()">
        <option value="0">Cool</option><option value="1">Fan</option>
        <option value="2">Dry</option><option value="3">Auto</option>
      </select>
      <button onclick="bumpTemp(1)">&plus;</button>
    </div>
  </div>
  <div class="muted" id="status">connecting…</div>
</main>
<script>
const RELAYS = ["Light 1", "Light 2", "Light 3", "Fan"];
let state = { relays: [false,false,false,false], acPower:false, acTemp:24, acMode:0 };
let pendingTemp = null;

function buildRelays() {
  const c = document.getElementById("relays");
  c.innerHTML = "";
  RELAYS.forEach((n, i) => {
    const row = document.createElement("div"); row.className = "row";
    row.innerHTML = `<span class="name">${n}</span>
      <label class="switch"><input type="checkbox" data-ch="${i}"><span class="slot"></span></label>`;
    row.querySelector("input").addEventListener("change", e =>
      setRelay(i, e.target.checked ? 1 : 0));
    c.appendChild(row);
  });
}

function render() {
  document.querySelectorAll('#relays input').forEach(el => {
    el.checked = !!state.relays[el.dataset.ch];
  });
  document.getElementById("acPower").checked = state.acPower;
  document.getElementById("acTemp").textContent = pendingTemp ?? state.acTemp;
  document.getElementById("acMode").value = state.acMode;
}

async function refresh() {
  try {
    const r = await fetch("/api/state");
    state = await r.json();
    pendingTemp = null;
    render();
    document.getElementById("status").textContent = "connected";
  } catch (e) {
    document.getElementById("status").textContent = "offline — retrying…";
  }
}

async function setRelay(ch, on) {
  await fetch(`/api/relay?ch=${ch}&state=${on}`, { method: "POST" });
  refresh();
}

function bumpTemp(d) {
  const cur = pendingTemp ?? state.acTemp;
  pendingTemp = Math.max(16, Math.min(30, cur + d));
  document.getElementById("acTemp").textContent = pendingTemp;
  sendAc();
}

async function sendAc() {
  const power = document.getElementById("acPower").checked ? 1 : 0;
  const temp  = pendingTemp ?? state.acTemp;
  const mode  = document.getElementById("acMode").value;
  await fetch(`/api/ac?power=${power}&temp=${temp}&mode=${mode}`, { method: "POST" });
  refresh();
}

document.getElementById("acPower").addEventListener("change", sendAc);
buildRelays();
refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTML";
