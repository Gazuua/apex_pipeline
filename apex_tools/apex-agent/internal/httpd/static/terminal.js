// Copyright (c) 2026 Gazuua. All rights reserved. Licensed under the MIT License.

var terminals = {};

function toggleTerminal(wsId) {
  var el = document.getElementById('term-' + wsId);
  if (!el) return;
  if (el.style.display === 'none') {
    el.style.display = 'block';
    if (!terminals[wsId]) {
      createTerminal(wsId, el);
    }
  } else {
    el.style.display = 'none';
  }
}

function createTerminal(wsId, container) {
  var term = new Terminal({
    fontSize: 13,
    fontFamily: '"Cascadia Code", "Consolas", monospace',
    theme: { background: '#1a1b26', foreground: '#c0caf5' },
    cursorBlink: true,
    scrollback: 5000,
  });
  var fit = new FitAddon.FitAddon();
  term.loadAddon(fit);
  term.open(container);
  fit.fit();

  var proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
  var ws = new WebSocket(proto + '//' + location.host + '/ws/session/' + wsId);
  ws.binaryType = 'arraybuffer';

  ws.onmessage = function(e) {
    term.write(new Uint8Array(e.data));
  };

  term.onData(function(data) {
    if (ws.readyState === WebSocket.OPEN) {
      ws.send(new TextEncoder().encode(data));
    }
  });

  ws.onclose = function() {
    term.write('\r\n\x1b[31m[Session disconnected]\x1b[0m\r\n');
  };

  ws.onerror = function() {
    term.write('\r\n\x1b[31m[Connection error]\x1b[0m\r\n');
  };

  window.addEventListener('resize', function() { fit.fit(); });

  terminals[wsId] = { term: term, ws: ws, fit: fit };
}

function startSession(wsId, mode) {
  fetch('/api/session/' + wsId + '/start?mode=' + mode, { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(data) {
      if (data.ok) {
        setTimeout(function() { toggleTerminal(wsId); }, 500);
      } else {
        alert('Start failed: ' + (data.error || 'unknown'));
      }
    })
    .catch(function(err) { alert('Session server unavailable'); });
}

function stopSession(wsId) {
  fetch('/api/session/' + wsId + '/stop', { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function() {
      if (terminals[wsId]) {
        terminals[wsId].ws.close();
        terminals[wsId].term.dispose();
        delete terminals[wsId];
        var el = document.getElementById('term-' + wsId);
        if (el) { el.style.display = 'none'; el.innerHTML = ''; }
      }
    });
}

function syncBranch(wsId) {
  fetch('/api/workspace/' + wsId + '/sync', { method: 'POST' });
}
