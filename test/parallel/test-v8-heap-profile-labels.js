// Flags: --expose-gc --sampling-heap-profiler-suppress-randomness
'use strict';
const common = require('../common');
const assert = require('assert');
const v8 = require('v8');

const retained = [];
function allocate(n = 2000) {
  for (let i = 0; i < n; i++) retained.push({ i, s: `value-${i}` });
}

function samplesWith(profile, predicate) {
  return profile.samples.filter((s) => predicate(s.labels));
}

// Option and argument validation.
assert.throws(() => v8.startHeapProfile({ labels: 1 }),
              { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => v8.withHeapProfileLabels(null, () => {}),
              { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => v8.withHeapProfileLabels({ a: 1 }, () => {}),
              { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => v8.withHeapProfileLabels({ a: 'b' }, null),
              { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => v8.setHeapProfileLabels('a'),
              { code: 'ERR_INVALID_ARG_TYPE' });
assert.strictEqual(v8.withHeapProfileLabels({ a: 'b' }, () => 42), 42);

// Without labels, samples carry a frozen empty labels object.
{
  const handle = v8.startHeapProfile({ sampleInterval: 64 });
  v8.withHeapProfileLabels({ route: '/ignored' }, () => allocate());
  const profile = handle.getAllocationProfile();
  assert.ok(profile.samples.length > 0);
  for (const sample of profile.samples) {
    assert.deepStrictEqual(sample.labels, {});
    assert.ok(Object.isFrozen(sample.labels));
    assert.strictEqual(typeof sample.nodeId, 'number');
    assert.strictEqual(typeof sample.size, 'number');
    assert.strictEqual(typeof sample.count, 'number');
    assert.strictEqual(typeof sample.sampleId, 'number');
  }
  assert.strictEqual(profile.externalBytes, undefined);
  assert.strictEqual(typeof handle.stop(), 'string');
  assert.strictEqual(handle.getAllocationProfile(), undefined);
  assert.strictEqual(handle.stop(), undefined);
}

// Samples carry the labels active at allocation, across await, and labels
// entered before the session starts.
(async () => {
  let release;
  const gate = new Promise((resolve) => { release = resolve; });
  const early = v8.withHeapProfileLabels({ phase: 'early' }, async () => {
    await gate;
    allocate();
  });

  const handle = v8.startHeapProfile({ sampleInterval: 64, labels: true });
  v8.withHeapProfileLabels({ route: '/sync', method: 'GET' }, () => allocate());
  await v8.withHeapProfileLabels({ route: '/async' }, async () => {
    await new Promise(setImmediate);
    allocate();
    // Nested labels replace the outer ones.
    v8.withHeapProfileLabels({ route: '/inner' }, () => allocate());
  });
  release();
  await early;
  await new Promise((resolve) => setImmediate(() => {
    v8.setHeapProfileLabels({ route: '/entered' });
    setImmediate(() => {
      allocate();
      resolve();
    });
  }));
  allocate();

  const profile = handle.getAllocationProfile();
  const sync = samplesWith(profile, (l) => l.route === '/sync');
  assert.ok(sync.length > 0);
  assert.deepStrictEqual(sync[0].labels, { route: '/sync', method: 'GET' });
  assert.ok(Object.isFrozen(sync[0].labels));
  // One labels object per label set.
  for (const sample of sync) assert.strictEqual(sample.labels, sync[0].labels);
  for (const route of ['/async', '/inner', '/entered']) {
    assert.ok(samplesWith(profile, (l) => l.route === route).length > 0, route);
  }
  assert.ok(samplesWith(profile, (l) => l.phase === 'early').length > 0);
  assert.ok(samplesWith(profile, (l) => Object.keys(l).length === 0).length > 0);
  assert.strictEqual(
    samplesWith(profile, (l) => l.route === '/inner' && l.method).length, 0);

  // Reads do not stop the session.
  allocate();
  assert.ok(handle.getAllocationProfile().samples.length >=
            profile.samples.length);

  assert.throws(() => v8.startHeapProfile(),
                { code: 'ERR_HEAP_PROFILE_HAVE_BEEN_STARTED' });
  handle.stop();
  assert.strictEqual(handle.getAllocationProfile(), undefined);

  // Label sets survive the loss of their holders while samples use them.
  const kept = v8.startHeapProfile({
    sampleInterval: 64,
    labels: true,
    includeObjectsCollectedByMajorGC: true,
  });
  for (let i = 0; i < 50; i++) {
    v8.withHeapProfileLabels({ iteration: `${i}` }, () => {
      const garbage = [];
      for (let j = 0; j < 200; j++) garbage.push({ j });
    });
    if (i % 10 === 0) {
      globalThis.gc();
      kept.getAllocationProfile();
    }
  }
  globalThis.gc();
  const seen = new Set();
  for (const { labels } of kept.getAllocationProfile().samples) {
    if (labels.iteration !== undefined) seen.add(labels.iteration);
  }
  assert.strictEqual(seen.size, 50);
  kept.stop();
})().then(common.mustCall());
