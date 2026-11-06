<!DOCTYPE html>
<!-- SPDX-License-Identifier: MIT -->
<html lang="en">
<head>
    <meta charset="utf-8">
    <title>[% nodename %] - H.264 Console</title>
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <link rel="stylesheet" href="/h264/h264.css?v=[% h264ver %]">
    <script type="text/javascript">
        if (typeof(PVE) === 'undefined') PVE = {};
        PVE.UserName = '[% username %]';
        PVE.CSRFPreventionToken = '[% token %]';
    </script>
    <script type="module" src="/h264/h264.js?v=[% h264ver %]"></script>
</head>
<body>
    <div id="bar">
        <span id="title">H.264 Console</span>
        <button id="cad" title="Send Ctrl+Alt+Del">Ctrl+Alt+Del</button>
        <select id="combos" title="Send key combination">
            <option value="">Send key…</option>
        </select>
        <button id="powerbtn" title="VM power: start, shutdown, reboot, stop, reset, suspend, resume" aria-expanded="false" aria-haspopup="menu"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="M8 1.5v6"/><path d="M4.6 3.6a5.5 5.5 0 1 0 6.8 0"/></svg> Power</button>
        <button id="settingsbtn" title="Stream settings: encoder, bitrate, frame rate, guest resolution (DPI)" aria-expanded="false"><svg width="12" height="12" viewBox="0 0 16 16" fill="currentColor" aria-hidden="true"><path d="M9.4 1l.4 2a5 5 0 0 1 1.3.7l1.9-.7 1.4 2.4-1.5 1.3a5 5 0 0 1 0 1.5l1.5 1.3-1.4 2.4-1.9-.7a5 5 0 0 1-1.3.8l-.4 2H6.6l-.4-2a5 5 0 0 1-1.3-.8l-1.9.7L1.6 10.5l1.5-1.3a5 5 0 0 1 0-1.5L1.6 6.4 3 4l1.9.7a5 5 0 0 1 1.3-.8l.4-2h2.8zM8 5.8a2.2 2.2 0 1 0 0 4.4 2.2 2.2 0 0 0 0-4.4z"/></svg> Stream</button>
        <button id="audiobtn" title="Sound"><svg width="13" height="12" viewBox="0 0 17 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round" aria-hidden="true"><path d="M2 6h3l4-3.5v11L5 10H2z" fill="currentColor" stroke-width="1.2"/><g id="audio-on"><path d="M11.5 5.5a3.5 3.5 0 0 1 0 5"/><path d="M13.5 3.5a6.3 6.3 0 0 1 0 9"/></g><g id="audio-off" style="display:none"><path d="M11.5 5.5l4 5M15.5 5.5l-4 5"/></g></svg> Sound</button>
        <button id="clipbtn" title="Clipboard shared with the guest" aria-expanded="false"><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" aria-hidden="true"><rect x="3" y="2.5" width="10" height="12" rx="1.5"/><path d="M6 2.5V1.5h4v1M5.5 6.5h5M5.5 9h5M5.5 11.5h3"/></svg> Clipboard</button>
        <label title="Scaling">
            <select id="scale">
                <option value="fit">scale: fit</option>
                <option value="down">scale: fit, no upscale</option>
                <option value="1">scale: 1:1 (CSS px)</option>
                <option value="dev">scale: 1:1 device px</option>
            </select>
        </label>
        <label title="Ask the guest (virtio-gpu) to match the window size"><input type="checkbox" id="autoresize"> Resize guest</label>
        <button id="resizehelpbtn" class="warn" title="The guest did not follow the window size: how to fix it" aria-expanded="false" hidden><svg width="12" height="12" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linejoin="round" stroke-linecap="round" aria-hidden="true"><path d="M8 1.8l6.6 12H1.4z"/><path d="M8 6.2v3.6M8 11.9v.1"/></svg> Fix resize</button>
        <label title="Relative-mouse guests only (e.g. BIOS, no USB tablet): lock the pointer to the console on click for exact, unbounded movement. Off = noVNC-style, pointer stays free."><input type="checkbox" id="capture"> Capture mouse</label>
        <label title="Show statistics overlay"><input type="checkbox" id="showstats"> Stats</label>
        <label title="Use the H.264 console for this browser's VM Console tab"><input type="checkbox" id="defconsole"> Default</label>
        <span class="spacer"></span>
        <span id="mousemode" class="dim"></span>
        <button id="novnc" title="Switch to the classic noVNC console">noVNC</button>
        <button id="fullscreen" title="Fullscreen (captures system keys in Chromium)"><svg width="12" height="12" viewBox="0 0 12 12" fill="none" stroke="currentColor" stroke-width="1.5"><path d="M1 4V1h3M8 1h3v3M11 8v3H8M4 11H1V8"/></svg></button>
    </div>
    <div id="wrap">
        <canvas id="screen" tabindex="0"></canvas>
        <div id="status">Loading…</div>
        <pre id="stats"></pre>
        <div id="panel" hidden></div>
        <div id="powermenu" role="menu" hidden></div>
        <div id="clippanel" hidden>
            <div class="p-head"><strong>Clipboard</strong><span id="clipstate" class="p-enc"></span><button id="clipclose" class="p-close" title="Close">×</button></div>
            <textarea id="cliptext" spellcheck="false" placeholder="The shared clipboard text appears here. Type or paste text to send it to the guest."></textarea>
            <div class="c-btns">
                <button id="clipsend" title="Put this text on the guest's clipboard">Send to guest</button>
                <button id="clippaste" title="Read your clipboard and send it to the guest">Send my clipboard</button>
                <button id="clipcopy" title="Put this text on your local clipboard">Copy here</button>
                <button id="cliptype" title="Type this text into the guest as key presses. Works without the spice agent (BIOS, installers, login screens); the guest's keyboard layout must be US">Type into guest</button>
            </div>
            <label class="c-auto" title="Ctrl+V in the console sends your clipboard to the guest first; text copied in the guest goes to your clipboard"><input type="checkbox" id="clipauto"> Sync automatically</label>
            <div class="p-note">Ctrl+V (Shift+Insert) in the console pastes your clipboard into the guest; text copied in the guest goes to your clipboard. Needs the spice agent in the guest (spice-vdagent, or the virtio-win guest tools). Without it, <em>Type into guest</em> types the box's text as key presses (US keyboard layout in the guest).</div>
        </div>
        <div id="resizepanel" hidden>
            <div class="p-head"><strong id="resizetitle">Resize guest</strong><span id="resizeinfo" class="p-enc"></span><button id="resizeclose" class="p-close" title="Close">×</button></div>
            <div id="resizebody"></div>
        </div>
        <div id="startdlg" hidden>
            <div id="startinfo">Guest not running</div>
            <button id="startbtn"><svg width="16" height="16" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><path d="M8 1.5v6"/><path d="M4.6 3.6a5.5 5.5 0 1 0 6.8 0"/></svg> Start Now</button>
        </div>
    </div>
</body>
</html>
