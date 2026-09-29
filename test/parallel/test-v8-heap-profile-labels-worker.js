'use strict';
const common = require('../common');
const assert = require('assert');
const { Worker } = require('worker_threads');

const worker = new Worker(`
  const assert = require('assert');
  const v8 = require('v8');
  const { parentPort } = require('worker_threads');
  const handle = v8.startHeapProfile({ sampleInterval: 64, labels: true });
  const retained = [];
  v8.withHeapProfileLabels({ thread: 'worker' }, () => {
    for (let i = 0; i < 2000; i++) retained.push({ i });
  });
  const { samples } = handle.getAllocationProfile();
  assert.ok(samples.some((s) => s.labels.thread === 'worker'));
  handle.stop();
  parentPort.postMessage('done');
`, { eval: true });

worker.on('message', common.mustCall((message) => {
  assert.strictEqual(message, 'done');
}));
worker.on('exit', common.mustCall((code) => {
  assert.strictEqual(code, 0);
}));

// A worker's own isolate can be profiled with labels while the main thread
// is profiled without them.
const v8 = require('v8');
const handle = v8.startHeapProfile();
worker.on('exit', () => handle.stop());
