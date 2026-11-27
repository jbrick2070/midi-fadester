/*
 * Visual Sidechain - the Tape Deck page (data/deck.html) run in Node with a stand-in video element and window
 * Copyright (C) 2026 Jeffrey A. Brick
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Usage: node test-deck-page.js <path to data/deck.html>
 */
'use strict';
const assert = require('assert');
const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2], 'utf8');
const match = html.match(/<script>([\s\S]*?)<\/script>/);
assert(match, 'deck.html has no <script>');

const posted = [];
const listeners = {};
const videoListeners = {};
const video = {
  getAttribute: () => null, removeAttribute() {}, load() {}, pause() {},
  addEventListener: (name, fn) => { videoListeners[name] = fn; },
  play: () => Promise.resolve(), currentTime: 0, duration: NaN, readyState: 0, paused: true, ended: false,
  playbackRate: 1, videoWidth: 0, videoHeight: 0, error: null, currentSrc: ''
};
const context = {
  document: { getElementById: () => video },
  window: { addEventListener: (name, fn) => { listeners[name] = fn; } },
  fetch: (url) => { posted.push(url); return Promise.resolve(); },
  setInterval: () => 0,
  Math, Date, JSON, Array, Number, String, isFinite, decodeURIComponent
};
vm.createContext(context);
vm.runInContext(match[1], context);
assert(listeners.tubester, 'the page listens for the tubester event');

function hello(port, token) {
  listeners.tubester({ detail: { hello: true, deck: 'd1', token: token, port: port } });
}

/* A port that is not a whole number from 1 to 65535 is ignored: "80@evil.example" would make the report address
 * http://127.0.0.1:80@evil.example/report, whose host is evil.example. */
const bad = ['80@evil.example', '48151/../x', 0, -1, 65536, 1.5, NaN, null, undefined, {}, '', ' 80'];
bad.forEach((port, i) => hello(port, 'bad' + i));
assert.deepStrictEqual(posted, [], 'no report goes out for a bad port: ' + JSON.stringify(posted));

/* a good port, as the filter sends it (a JSON number), reports to the loopback only */
hello(48151, 'good');
assert.deepStrictEqual(posted, ['http://127.0.0.1:48151/report']);

/* a bad hello after a good one leaves the link as it was: the next report still goes to the loopback */
hello('80@evil.example', 'later');
videoListeners.pause();
assert.deepStrictEqual(posted, ['http://127.0.0.1:48151/report', 'http://127.0.0.1:48151/report']);

console.log('test-deck-page: all passed');
