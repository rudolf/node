'use strict';
const common = require('../common');
common.skipIfInspectorDisabled();
const assert = require('assert');
const { Session } = require('inspector');
const v8 = require('v8');

const retained = [];
function allocate() {
  for (let i = 0; i < 2000; i++) retained.push({ i });
}

const session = new Session();
session.connect();

const old = v8.startHeapProfile({ sampleInterval: 64, labels: true });
v8.withHeapProfileLabels({ session: 'old' }, allocate);

// The inspector can stop a sampler it did not start. The handle then reads
// as stopped, and a new start supersedes it.
session.post('HeapProfiler.stopSampling', common.mustSucceed(() => {
  assert.strictEqual(old.getAllocationProfile(), undefined);

  const next = v8.startHeapProfile({ sampleInterval: 64, labels: true });
  v8.withHeapProfileLabels({ session: 'next' }, allocate);
  assert.strictEqual(old.getAllocationProfile(), undefined);
  assert.strictEqual(old.stop(), undefined);

  const { samples } = next.getAllocationProfile();
  assert.ok(samples.some((s) => s.labels.session === 'next'));
  assert.ok(!samples.some((s) => s.labels.session === 'old'));
  assert.strictEqual(typeof next.stop(), 'string');
  session.disconnect();
}));
