(function() {
  var Module = window.Module || {};
  window.Module = Module;

  Module.onBleNotify = function(jsonPayload) {
    if (window.store) {
      window.store.bleStatus = 'BLE GATT Notify Response: ' + jsonPayload;
    }
    console.log('[WASM_BLE] 📤 Transmitting GATT Notify on Char (e2e5e5e3): ' + jsonPayload);
  };

  window.requestWebBluetooth = function() {
    console.log('[WEB_BT] 🔵 Requesting browser Bluetooth Low Energy scan (filtering Service UUID e2e5e5e0-1234-5678-1234-56789abcdef0)...');
    if (!navigator.bluetooth) {
      console.error('[WEB_BT] Web Bluetooth API is not available in this browser context (requires secure HTTPS/localhost Chrome/Edge).');
      return;
    }
    navigator.bluetooth.requestDevice({
      filters: [{ services: ['e2e5e5e0-1234-5678-1234-56789abcdef0'] }]
    }).then(function(device) {
      console.log('[WEB_BT] ✓ Paired with physical StackChan device: ' + device.name + ' (' + device.id + ')');
    }).catch(function(err) {
      console.error('[WEB_BT] Web Bluetooth pairing error/cancelled: ' + err);
    });
  };

  window.sendBleHandshake = function() {
    var payload = JSON.stringify({ cmd: "handshake", data: "StackChanWorldMobileApp_v2" });
    console.log('[WASM_BLE] 📥 Simulating Mobile App GATT Write on Char (e2e5e5e3): ' + payload);
    if (Module.ccall) {
      Module.ccall('wasm_ble_send_config_json', null, ['string'], [payload]);
    }
  };

  window.sendBleWifi = function() {
    var ssid = (window.store && window.store.wifiSsid) ? window.store.wifiSsid : 'StackChan_World_AP';
    var pass = (window.store && window.store.wifiPass) ? window.store.wifiPass : 'stackchan2026';
    var payload = JSON.stringify({ cmd: "setWifi", data: { ssid: ssid, password: pass } });
    console.log('[WASM_BLE] 📥 Simulating Mobile App Wi-Fi Provisioning Write on Char (e2e5e5e3): ' + payload);
    if (Module.ccall) {
      Module.ccall('wasm_ble_send_config_json', null, ['string'], [payload]);
    }
  };

  window.sendBleStatus = function() {
    var payload = JSON.stringify({ cmd: "getWifiStatus" });
    console.log('[WASM_BLE] 📥 Querying BLE Wi-Fi Provisioning Status...');
    if (Module.ccall) {
      Module.ccall('wasm_ble_send_config_json', null, ['string'], [payload]);
    }
  };
})();
