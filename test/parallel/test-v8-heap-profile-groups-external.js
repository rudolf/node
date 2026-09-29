// Flags: --expose-gc
'use strict';
const common = require('../common');
const assert = require('assert');
const { setImmediate } = require('timers/promises');
const v8 = require('v8');

const kMiB = 1024 * 1024;

function group(handle, tenant, options) {
  const stats = handle.getHeapStats(options);
  const found = stats.groups.find((g) => g.labels.tenant === tenant);
  return { stats, group: found };
}

(async () => {
  const handle = v8.startHeapProfile({
    sampleInterval: kMiB, labels: true, groupBy: ['tenant'],
  });
  let kept = v8.withHeapProfileLabels({ tenant: 'a' }, () => [
    new ArrayBuffer(kMiB),
    new ArrayBuffer(kMiB / 2),
  ]);
  const unlabelled = new ArrayBuffer(kMiB);

  let { stats, group: a } = group(handle, 'a');
  assert.strictEqual(a.allocatedExternalBytes, BigInt(kMiB * 1.5));
  assert.strictEqual(a.allocatedExternalCount, 2n);
  assert.strictEqual(a.currentExternalBytes, BigInt(kMiB * 1.5));
  assert.strictEqual(a.freedExternalCount, 0n);
  assert.strictEqual(a.publishedPeakExternalBytes, 0n);
  const unknown = stats.groups.find((g) => g.isUnknown);
  assert.ok(unknown.allocatedExternalBytes >= BigInt(kMiB));

  // Only advancePeakWindow closes the peak window.
  ({ stats, group: a } = group(handle, 'a'));
  assert.strictEqual(stats.peakWindowId, 0n);
  ({ stats, group: a } = group(handle, 'a', { advancePeakWindow: true }));
  assert.strictEqual(stats.peakWindowId, 1n);
  assert.strictEqual(a.publishedPeakExternalBytes, BigInt(kMiB * 1.5));

  // Frees credit the group captured at allocation, even when they happen on
  // V8's sweeper thread after GC.
  kept = null;
  await v8.withHeapProfileLabels({ tenant: 'b' }, async () => {
    for (let i = 0; i < 100 && group(handle, 'a').group.freedExternalCount < 2n;
      i++) {
      globalThis.gc();
      await setImmediate();
    }
  });
  ({ group: a } = group(handle, 'a'));
  assert.strictEqual(a.freedExternalCount, 2n);
  assert.strictEqual(a.freedExternalBytes, BigInt(kMiB * 1.5));
  assert.strictEqual(a.currentExternalBytes, 0n);
  assert.strictEqual(a.publishedPeakExternalBytes, BigInt(kMiB * 1.5));

  // The window after the first one started at the live bytes at that time.
  ({ group: a } = group(handle, 'a', { advancePeakWindow: true }));
  assert.strictEqual(a.publishedPeakExternalBytes, BigInt(kMiB * 1.5));
  ({ stats, group: a } = group(handle, 'a', { advancePeakWindow: true }));
  assert.strictEqual(stats.peakWindowId, 3n);
  assert.strictEqual(a.publishedPeakExternalBytes, 0n);

  handle.stop();
  assert.strictEqual(kept, null);
  assert.strictEqual(unlabelled.byteLength, kMiB);
})().then(common.mustCall());
