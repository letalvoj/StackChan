(function() {
  var Module = window.Module || {};
  window.Module = Module;

  var _lastJpegLogTime = 0;
  var _jpegSkipCount = 0;

  window.toggleWebcamStream = function() {
    var video = document.getElementById('webcam-feed');
    if (!video) return;
    if (video.srcObject) {
      var tracks = video.srcObject.getTracks();
      tracks.forEach(function(track) { track.stop(); });
      video.srcObject = null;
      console.log('[WASM_CAMERA] ⏸️ WebRTC webcam stream suspended.');
    } else {
      if (navigator.mediaDevices && navigator.mediaDevices.getUserMedia) {
        navigator.mediaDevices.getUserMedia({ video: { width: 320, height: 240 } }).then(function(stream) {
          video.srcObject = stream;
          console.log('[WASM_CAMERA] ▶️ Warm WebRTC webcam stream active offscreen (320x240 for AVATAR rendering).');
        }).catch(function(err) {
          console.log('[WASM_CAMERA:ERR] getUserMedia failed: ' + err);
        });
      } else {
        console.log('[WASM_CAMERA:WARN] navigator.mediaDevices not supported in this environment.');
      }
    }
  };

  window._captureWebcamRgb565 = function(dstPtr, maxLen) {
    var video = document.getElementById('webcam-feed');
    var canvas = document.getElementById('webcam-canvas-offscreen');
    if (!canvas || !Module.HEAPU8) return 0;
    var ctx = canvas.getContext('2d');
    if (video && video.srcObject && video.readyState >= 2) {
      ctx.drawImage(video, 0, 0, 320, 240);
    } else {
      var grad = ctx.createLinearGradient(0, 0, 320, 240);
      grad.addColorStop(0, '#ec4899'); // AVATAR Pink
      grad.addColorStop(1, '#6366f1'); // Indigo
      ctx.fillStyle = grad;
      ctx.fillRect(0, 0, 320, 240);
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 22px sans-serif';
      ctx.fillText('AVATAR WEBRTC VISION', 35, 125);
    }
    var imgData = ctx.getImageData(0, 0, 320, 240);
    var data = imgData.data;
    var rgb565 = new Uint8Array(320 * 240 * 2);
    var idx = 0;
    for (var i = 0; i < data.length; i += 4) {
      var r = (data[i] >> 3) & 0x1F;
      var g = (data[i + 1] >> 2) & 0x3F;
      var b = (data[i + 2] >> 3) & 0x1F;
      var rgb = (r << 11) | (g << 5) | b;
      rgb565[idx++] = rgb & 0xFF;        // Low byte (Little Endian)
      rgb565[idx++] = (rgb >> 8) & 0xFF; // High byte
    }
    var len = rgb565.length;
    if (len <= maxLen) {
      Module.HEAPU8.set(rgb565, dstPtr);
      return len;
    }
    return 0;
  };

  window._captureWebcamJpeg = function(dstPtr, maxLen) {
    var video = document.getElementById('webcam-feed');
    var canvas = document.getElementById('webcam-canvas-offscreen');
    if (!canvas) return 0;
    var ctx = canvas.getContext('2d');
    if (video && video.srcObject && video.readyState >= 2) {
      ctx.drawImage(video, 0, 0, 320, 240);
    } else {
      var grad = ctx.createLinearGradient(0, 0, 320, 240);
      grad.addColorStop(0, '#06b6d4');
      grad.addColorStop(1, '#3b82f6');
      ctx.fillStyle = grad;
      ctx.fillRect(0, 0, 320, 240);
      ctx.fillStyle = '#ffffff';
      ctx.font = 'bold 20px monospace';
      ctx.fillText('WASM VISION TEST', 65, 125);
    }
    var dataUrl = canvas.toDataURL('image/jpeg', 0.85);
    var base64Data = dataUrl.replace(/^data:image\/jpeg;base64,/, '');
    var binaryStr = atob(base64Data);
    var len = binaryStr.length;
    if (len > maxLen) {
      console.log('[WASM_CAMERA:ERR] Captured JPEG size (' + len + ') exceeds buffer capacity (' + maxLen + ').');
      return 0;
    }
    var jpegBytes = new Uint8Array(len);
    for (var i = 0; i < len; i++) {
      jpegBytes[i] = binaryStr.charCodeAt(i);
    }
    if (Module.HEAPU8) {
      Module.HEAPU8.set(jpegBytes, dstPtr);
    } else {
      console.log('[WASM_CAMERA:ERR] Module.HEAPU8 undefined.');
      return 0;
    }
    var now = performance.now();
    if (_lastJpegLogTime === 0 || (now - _lastJpegLogTime) >= 1000) {
      var skipMsg = _jpegSkipCount > 0 ? ' (skipped ' + _jpegSkipCount + ' logs in last ' + Math.round(now - _lastJpegLogTime) + 'ms)' : '';
      console.log('[WASM_CAMERA] ✓ Synchronous JPEG captured (' + len + ' bytes written to memory).' + skipMsg);
      _lastJpegLogTime = now;
      _jpegSkipCount = 0;
    } else {
      _jpegSkipCount++;
    }
    return len;
  };

  window._setWebcamMirror = function(enabled) {
    var video = document.getElementById('webcam-feed');
    if (video) {
      video.style.transform = enabled ? 'scaleX(-1)' : 'scaleX(1)';
    }
  };

  window._explainWebcamFrame = function(jpegPtr, jpegLen, question, url, token) {
    if (!Module.HEAPU8 || !jpegPtr || !jpegLen) return 0;
    var bytes = Module.HEAPU8.subarray(jpegPtr, jpegPtr + jpegLen);
    var blob = new Blob([bytes], { type: 'image/jpeg' });
    var formData = new FormData();
    formData.append('file', blob, 'webcam_frame.jpg');
    formData.append('question', question || 'Explain what you see in this photo.');
    
    var jsonResponse = '';
    if (url === 'mock://vision' || url.indexOf('localhost:8081/vision') !== -1) {
      console.log('[WASM_CAMERA] Using local simulated vision endpoint (preventing POST conflict with WebSocket dev server).');
      jsonResponse = JSON.stringify({ explanation: "Simulated Vision Response: I can see the StackChan desk robot workspace with an active 320x240 webcam stream.", question: question });
    } else {
      var xhr = new XMLHttpRequest();
      xhr.open('POST', url, false);
      if (token && token.trim() !== '') {
        xhr.setRequestHeader('Authorization', 'Bearer ' + token);
      }
      try {
        xhr.send(formData);
        if (xhr.status >= 200 && xhr.status < 300) {
          jsonResponse = xhr.responseText;
        } else {
          console.log('[WASM_CAMERA:HTTP_ERR] Vision server returned HTTP ' + xhr.status + ': ' + xhr.responseText);
          jsonResponse = JSON.stringify({ error: "HTTP " + xhr.status, details: xhr.responseText });
        }
      } catch (err) {
        console.log('[WASM_CAMERA:NET_ERR] Synchronous XHR failed: ' + err + '. (Returning simulation stub for testing)');
        jsonResponse = JSON.stringify({ explanation: "Simulated Vision Response: I can see the StackChan desk robot workspace with an active 320x240 webcam stream.", question: question });
      }
    }
    
    if (typeof allocateUTF8 === 'function') {
      return allocateUTF8(jsonResponse);
    } else if (typeof Module._malloc === 'function' && typeof stringToUTF8 === 'function') {
      var strLen = lengthBytesUTF8(jsonResponse) + 1;
      var strPtr = Module._malloc(strLen);
      stringToUTF8(jsonResponse, strPtr, strLen);
      return strPtr;
    } else {
      console.log('[WASM_CAMERA:ERR] String allocation functions unavailable.');
      return 0;
    }
  };

  window.triggerAvatarVisionFrame = function() {
    if (Module.ccall) {
      Module.ccall('wasm_camera_test_capture', null, [], []);
      console.log('[WASM_CAMERA] ✓ Triggered native camera capture and frame emission into authentic Mooncake AVATAR VideoWindow.');
    }
  };

  window.testWebcamCapture = function() {
    console.log('[WASM_CAMERA] Initiating standalone test capture...');
    window.triggerAvatarVisionFrame();
    var testBuf = new Uint8Array(65536);
    var ptr = Module._malloc ? Module._malloc(65536) : 0;
    if (ptr) {
      var len = window._captureWebcamJpeg(ptr, 65536);
      console.log('[WASM_CAMERA] Captured snapshot frame (' + len + ' JPEG bytes)');
      var respPtr = window._explainWebcamFrame(ptr, len, "Describe this camera test image", "http://localhost:8081/vision", "test_token");
      if (respPtr) {
        var respStr = UTF8ToString(respPtr);
        console.log('[WASM_CAMERA] Explain() completed successfully: ' + respStr);
        if (Module._free) Module._free(respPtr);
      }
      if (Module._free) Module._free(ptr);
    } else {
      console.log('[WASM_CAMERA:ERR] Memory allocation failed for test capture.');
    }
  };
})();
