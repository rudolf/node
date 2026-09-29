// Flags: --expose-gc
'use strict';
const common = require('../common');
const assert = require('assert');
const { setImmediate } = require('timers/promises');
const v8 = require('v8');

const kMiB = 1024 * 1024;

function external(handle, store) {
  const { externalBytes = [] } = handle.getAllocationProfile();
  return externalBytes.find((entry) => entry.labels.store === store);
}

(async () => {
  const handle = v8.startHeapProfile({ sampleInterval: kMiB, labels: true });
  let kept = v8.withHeapProfileLabels({ store: 'a' }, () => [
    Buffer.allocUnsafeSlow(kMiB),
    new ArrayBuffer(kMiB / 2),
  ]);
  v8.withHeapProfileLabels({ store: 'b' }, () => Buffer.allocUnsafeSlow(kMiB));
  const unlabelled = Buffer.allocUnsafeSlow(kMiB);

  const a = external(handle, 'a');
  assert.strictEqual(a.bytes, kMiB * 1.5);
  assert.deepStrictEqual(a.labels, { store: 'a' });
  assert.ok(Object.isFrozen(a.labels));
  for (const { labels } of handle.getAllocationProfile().externalBytes) {
    assert.notDeepStrictEqual(labels, {});
  }

  // Backing stores may be freed on V8's sweeper thread after GC.
  kept = null;
  for (let i = 0; i < 100 && external(handle, 'a') !== undefined; i++) {
    globalThis.gc();
    await setImmediate();
  }
  assert.strictEqual(external(handle, 'a'), undefined);
  assert.strictEqual(kept, null);
  assert.strictEqual(unlabelled.length, kMiB);

  handle.stop();
  // Stopped sessions do not track allocations.
  v8.withHeapProfileLabels({ store: 'c' }, () => Buffer.allocUnsafeSlow(kMiB));
})().then(common.mustCall());
