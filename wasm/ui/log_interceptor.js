(function() {
  window.addEventListener('DOMContentLoaded', function() {
    var logPane = document.getElementById('log-pane');
    if (!logPane) return;
    window._appendLog = function(msg) {
      var div = document.createElement('div');
      var cls = 'log-entry ';
      if (msg.indexOf('[RADAR]') !== -1) cls += 'log-radar';
      else if (msg.indexOf('[WASM_SERVO]') !== -1) cls += 'log-servo';
      else if (msg.indexOf('[WASM_BLE]') !== -1 || msg.indexOf('[WEB_BT]') !== -1) cls += 'log-ble';
      else if (msg.indexOf('[INFO]') !== -1 || msg.indexOf('[WASM_') !== -1 || msg.indexOf('info') !== -1) cls += 'log-info';
      div.className = cls;
      div.textContent = new Date().toISOString().split('T')[1].slice(0, 8) + ' ' + msg;
      logPane.appendChild(div);
      logPane.scrollTop = logPane.scrollHeight;
    };
  });

  var originalLog = console.log;
  console.log = function() {
    originalLog.apply(console, arguments);
    if (window._appendLog) {
      window._appendLog(Array.from(arguments).join(' '));
    }
  };
  var originalError = console.error;
  console.error = function() {
    originalError.apply(console, arguments);
    if (window._appendLog) {
      window._appendLog('[ERROR] ' + Array.from(arguments).join(' '));
    }
  };
})();
