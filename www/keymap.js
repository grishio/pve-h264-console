// SPDX-License-Identifier: MIT
// KeyboardEvent.code -> QEMU "qnum" (XT set-1 scancode, 0xE0-prefixed keys
// encoded as 0x80 | code). Layout independent: the guest applies its own
// keyboard layout, exactly like a physical PS/2 keyboard.
export const QNUM = {
    Escape: 0x01, Digit1: 0x02, Digit2: 0x03, Digit3: 0x04, Digit4: 0x05,
    Digit5: 0x06, Digit6: 0x07, Digit7: 0x08, Digit8: 0x09, Digit9: 0x0a,
    Digit0: 0x0b, Minus: 0x0c, Equal: 0x0d, Backspace: 0x0e, Tab: 0x0f,
    KeyQ: 0x10, KeyW: 0x11, KeyE: 0x12, KeyR: 0x13, KeyT: 0x14, KeyY: 0x15,
    KeyU: 0x16, KeyI: 0x17, KeyO: 0x18, KeyP: 0x19, BracketLeft: 0x1a,
    BracketRight: 0x1b, Enter: 0x1c, ControlLeft: 0x1d, KeyA: 0x1e,
    KeyS: 0x1f, KeyD: 0x20, KeyF: 0x21, KeyG: 0x22, KeyH: 0x23, KeyJ: 0x24,
    KeyK: 0x25, KeyL: 0x26, Semicolon: 0x27, Quote: 0x28, Backquote: 0x29,
    ShiftLeft: 0x2a, Backslash: 0x2b, KeyZ: 0x2c, KeyX: 0x2d, KeyC: 0x2e,
    KeyV: 0x2f, KeyB: 0x30, KeyN: 0x31, KeyM: 0x32, Comma: 0x33,
    Period: 0x34, Slash: 0x35, ShiftRight: 0x36, NumpadMultiply: 0x37,
    AltLeft: 0x38, Space: 0x39, CapsLock: 0x3a, F1: 0x3b, F2: 0x3c,
    F3: 0x3d, F4: 0x3e, F5: 0x3f, F6: 0x40, F7: 0x41, F8: 0x42, F9: 0x43,
    F10: 0x44, NumLock: 0x45, ScrollLock: 0x46, Numpad7: 0x47,
    Numpad8: 0x48, Numpad9: 0x49, NumpadSubtract: 0x4a, Numpad4: 0x4b,
    Numpad5: 0x4c, Numpad6: 0x4d, NumpadAdd: 0x4e, Numpad1: 0x4f,
    Numpad2: 0x50, Numpad3: 0x51, Numpad0: 0x52, NumpadDecimal: 0x53,
    IntlBackslash: 0x56, F11: 0x57, F12: 0x58, NumpadEqual: 0x59,
    F13: 0x64, F14: 0x65, F15: 0x66, F16: 0x67, F17: 0x68, F18: 0x69,
    F19: 0x6a, F20: 0x6b, F21: 0x6c, F22: 0x6d, F23: 0x6e, KanaMode: 0x70,
    Lang2: 0x71, Lang1: 0x72, IntlRo: 0x73, F24: 0x76, Convert: 0x79,
    NonConvert: 0x7b, IntlYen: 0x7d, NumpadComma: 0x7e,
    // 0xE0 xx  ->  0x80 | xx
    MediaTrackPrevious: 0x90, MediaTrackNext: 0x99, NumpadEnter: 0x9c,
    ControlRight: 0x9d, AudioVolumeMute: 0xa0, LaunchApp2: 0xa1,
    MediaPlayPause: 0xa2, MediaStop: 0xa4, AudioVolumeDown: 0xae,
    AudioVolumeUp: 0xb0, BrowserHome: 0xb2, NumpadDivide: 0xb5,
    PrintScreen: 0xb7, AltRight: 0xb8, Pause: 0xc6, Home: 0xc7,
    ArrowUp: 0xc8, PageUp: 0xc9, ArrowLeft: 0xcb, ArrowRight: 0xcd,
    End: 0xcf, ArrowDown: 0xd0, PageDown: 0xd1, Insert: 0xd2,
    Delete: 0xd3, MetaLeft: 0xdb, MetaRight: 0xdc, ContextMenu: 0xdd,
    Power: 0xde, Sleep: 0xdf, WakeUp: 0xe3, BrowserSearch: 0xe5,
    BrowserFavorites: 0xe6, BrowserRefresh: 0xe7, BrowserStop: 0xe8,
    BrowserForward: 0xe9, BrowserBack: 0xea, LaunchApp1: 0xeb,
    LaunchMail: 0xec, MediaSelect: 0xed,
    // legacy names some browsers still emit
    OSLeft: 0xdb, OSRight: 0xdc,
};

// Key combos offered in the "Send key" menu.
export const COMBOS = [
    ['Ctrl+Alt+Del', ['ControlLeft', 'AltLeft', 'Delete']],
    ['Ctrl+Alt+Backspace', ['ControlLeft', 'AltLeft', 'Backspace']],
    ['Ctrl+Alt+F1', ['ControlLeft', 'AltLeft', 'F1']],
    ['Ctrl+Alt+F2', ['ControlLeft', 'AltLeft', 'F2']],
    ['Ctrl+Alt+F7', ['ControlLeft', 'AltLeft', 'F7']],
    ['Alt+Tab', ['AltLeft', 'Tab']],
    ['Alt+F4', ['AltLeft', 'F4']],
    ['Windows / Super', ['MetaLeft']],
    ['Print Screen', ['PrintScreen']],
    ['Escape', ['Escape']],
];
