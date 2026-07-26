(function() {
  var Module = window.Module || {};
  window.Module = Module;

  var lastRadarLogYaw = null, lastRadarLogPitch = null;

  Module.updateServoRadar = function(yaw, pitch) {
    var radarCvs = document.getElementById('radar');
    var radarCtx = radarCvs ? radarCvs.getContext('2d') : null;
    if (!radarCtx) return;
    var w = radarCvs.width, h = radarCvs.height;
    
    radarCtx.fillStyle = '#010e05';
    radarCtx.fillRect(0, 0, w, h);

    radarCtx.fillStyle = '#002810';
    for (var x = 10; x < w; x += 20) {
      for (var y = 10; y < h; y += 20) {
        radarCtx.fillRect(x, y, 2, 2);
      }
    }

    var cx = w / 2, cy = h - 30;
    var maxR = 150;

    radarCtx.strokeStyle = '#005522';
    radarCtx.lineWidth = 1;
    for (var r = 30; r <= maxR; r += 40) {
      radarCtx.beginPath();
      radarCtx.arc(cx, cy, r, Math.PI, 2 * Math.PI);
      radarCtx.stroke();
    }

    var angles = [-120, -90, -60, -30, 0, 30, 60, 90, 120];
    for (var i = 0; i < angles.length; i++) {
      var angRad = (angles[i] - 90) * Math.PI / 180;
      var x2 = cx + maxR * Math.cos(angRad);
      var y2 = cy + maxR * Math.sin(angRad);
      radarCtx.beginPath();
      radarCtx.moveTo(cx, cy);
      radarCtx.lineTo(x2, y2);
      radarCtx.stroke();

      if (Math.abs(angles[i]) <= 90 || angles[i] === -120 || angles[i] === 120) {
        radarCtx.fillStyle = '#00ff66';
        radarCtx.font = '10px monospace';
        var tx = cx + (maxR + 10) * Math.cos(angRad) - 10;
        var ty = cy + (maxR + 10) * Math.sin(angRad) + 4;
        radarCtx.fillText(angles[i] + '°', tx, ty);
      }
    }

    var targetAngleRad = (yaw - 90) * Math.PI / 180;
    var targetR = maxR * (0.3 + (Math.abs(pitch) / 90.0) * 0.7);
    var dotX = cx + targetR * Math.cos(targetAngleRad);
    var dotY = cy + targetR * Math.sin(targetAngleRad);

    radarCtx.strokeStyle = '#00ff66';
    radarCtx.lineWidth = 2;
    radarCtx.setLineDash([4, 2]);
    radarCtx.beginPath();
    radarCtx.moveTo(cx, cy);
    radarCtx.lineTo(dotX, dotY);
    radarCtx.stroke();
    radarCtx.setLineDash([]);

    radarCtx.fillStyle = '#ff2222';
    radarCtx.beginPath();
    radarCtx.arc(dotX, dotY, 8, 0, 2 * Math.PI);
    radarCtx.fill();
    radarCtx.strokeStyle = '#ffffff';
    radarCtx.lineWidth = 2;
    radarCtx.stroke();

    radarCtx.fillStyle = '#00ff66';
    radarCtx.font = 'bold 13px monospace';
    radarCtx.fillText('YAW: ' + (yaw > 0 ? '+' : '') + yaw.toFixed(1) + '°', 12, 22);
    radarCtx.fillText('PITCH: ' + (pitch > 0 ? '+' : '') + pitch.toFixed(1) + '°', 180, 22);

    if (Math.abs(yaw) > 60 || Math.abs(pitch) > 60) {
      radarCtx.fillStyle = '#ffcc00';
      radarCtx.font = 'italic bold 13px sans-serif';
      radarCtx.fillText(yaw < 0 ? '*SWOOSH LEFT!*' : (yaw > 0 ? '*SWOOSH RIGHT!*' : '*LOOK UP!*'), cx - 48, cy - 10);
    } else if (yaw === 0 && pitch === 0) {
      radarCtx.fillStyle = '#00ff66';
      radarCtx.font = '11px monospace';
      radarCtx.fillText('[ HOME LOCKED ]', cx - 44, cy - 10);
    }

    if (typeof Module._radarLogCount === 'undefined') Module._radarLogCount = 0;
    if (lastRadarLogYaw === null || Math.abs(lastRadarLogYaw - yaw) >= 15.0 || Math.abs(lastRadarLogPitch - pitch) >= 15.0) {
      if (Module._radarLogCount < 3) {
        Module._radarLogCount++;
        console.log('[RADAR] 🎯 Head servo moved -> YAW: ' + (yaw > 0 ? '+' : '') + yaw.toFixed(1) + '° | PITCH: ' + (pitch > 0 ? '+' : '') + pitch.toFixed(1) + '°');
        if (Module._radarLogCount === 3) {
          console.log('[RADAR] (Throttling further routine radar movement logs — UI canvas continues tracking smoothly)');
        }
      }
      lastRadarLogYaw = yaw;
      lastRadarLogPitch = pitch;
    }
  };

  window.addEventListener('DOMContentLoaded', function() {
    Module.updateServoRadar(0, 0);
  });
})();
