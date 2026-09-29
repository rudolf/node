// Flags: --sampling-heap-profiler-suppress-randomness
'use strict';
require('../common');
const assert = require('assert');
const { isDeepStrictEqual } = require('util');
const v8 = require('v8');

const retained = [];
function allocate(n = 2000) {
  for (let i = 0; i < n; i++) retained.push({ i, s: `value-${i}` });
}

function findGroup(stats, labels) {
  return stats.groups.find((g) => !g.isUnknown && !g.isOverflow &&
    isDeepStrictEqual(g.labels, labels));
}

// Option validation.
for (const [options, code] of [
  [{ groupBy: ['a'] }, 'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, groupBy: 'a' }, 'ERR_INVALID_ARG_TYPE'],
  [{ labels: true, groupBy: [] }, 'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, groupBy: ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i'] },
   'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, groupBy: [1] }, 'ERR_INVALID_ARG_TYPE'],
  [{ labels: true, groupBy: [''] }, 'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, groupBy: ['é'.repeat(128)] }, 'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, groupBy: ['a', 'a'] }, 'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, maxGroups: 1 }, 'ERR_INVALID_ARG_VALUE'],
  [{ labels: true, groupBy: ['a'], maxGroups: 0 }, 'ERR_OUT_OF_RANGE'],
  [{ labels: true, groupBy: ['a'], maxGroups: 65537 }, 'ERR_OUT_OF_RANGE'],
  [{ labels: true, groupBy: ['a'], maxGroups: 1.5 }, 'ERR_OUT_OF_RANGE'],
  [{ labels: true, groupBy: ['a'], maxGroups: '1' }, 'ERR_INVALID_ARG_TYPE'],
]) {
  assert.throws(() => v8.startHeapProfile(options), { code });
}

// Without groupBy, getHeapStats() returns undefined.
{
  const handle = v8.startHeapProfile({ labels: true });
  assert.strictEqual(handle.getHeapStats(), undefined);
  handle.stop();
}

{
  const groupBy = ['tenant', 'task'];
  const handle = v8.startHeapProfile({
    sampleInterval: 256, labels: true, groupBy, maxGroups: 2,
  });
  assert.throws(() => handle.getHeapStats({ advancePeakWindow: 1 }),
                { code: 'ERR_INVALID_ARG_TYPE' });

  // Other keys do not split a group, and key order follows groupBy.
  v8.withHeapProfileLabels({ task: 't', tenant: 'a', route: '/1' }, allocate);
  v8.withHeapProfileLabels({ tenant: 'a', task: 't', route: '/2' }, allocate);
  // Missing and empty values are omitted.
  v8.withHeapProfileLabels({ tenant: 'b', task: '' }, allocate);
  // No groupBy key present: the unknown group.
  v8.withHeapProfileLabels({ route: '/3' }, allocate);
  v8.withHeapProfileLabels({ tenant: '' }, allocate);
  allocate();
  // A value over 255 UTF-8 bytes, or maxGroups exhausted: the overflow group.
  v8.withHeapProfileLabels({ tenant: 'é'.repeat(128) }, allocate);
  v8.withHeapProfileLabels({ tenant: 'c' }, allocate);

  const stats = handle.getHeapStats();
  assert.strictEqual(stats.sampleInterval, 256);
  assert.strictEqual(stats.peakWindowId, 0n);
  assert.strictEqual(stats.overflowCount, 2n);
  assert.strictEqual(stats.groups.length, 4);

  const [unknown, overflow] = stats.groups;
  assert.strictEqual(unknown.isUnknown, true);
  assert.strictEqual(overflow.isOverflow, true);
  assert.deepStrictEqual(unknown.labels, {});
  assert.deepStrictEqual(overflow.labels, {});
  const at = findGroup(stats, { tenant: 'a', task: 't' });
  assert.deepStrictEqual(Object.keys(at.labels), groupBy);
  const b = findGroup(stats, { tenant: 'b' });

  for (const group of stats.groups) {
    assert.ok(Object.isFrozen(group.labels));
    assert.ok(group.allocatedHeapSampleCount > 0n);
    assert.ok(group.currentHeapSampleCount > 0n);
    assert.ok(group.currentHeapSampleCount <= group.allocatedHeapSampleCount);
    assert.ok(group.currentHeapBytes >= group.currentHeapSampleCount * 256n);
    for (const key of ['allocatedExternalBytes', 'allocatedExternalCount',
                       'freedExternalBytes', 'freedExternalCount',
                       'currentExternalBytes', 'publishedPeakExternalBytes']) {
      assert.strictEqual(typeof group[key], 'bigint');
    }
  }
  // Two label sets share the group of { tenant: 'a', task: 't' }.
  assert.ok(at.allocatedHeapSampleCount > b.allocatedHeapSampleCount);

  // Samples keep the labels of their label set.
  const routes = new Set(handle.getAllocationProfile().samples
    .map((s) => s.labels.route).filter(Boolean));
  assert.deepStrictEqual([...routes].sort(), ['/1', '/2', '/3']);

  handle.stop();
  assert.strictEqual(handle.getHeapStats(), undefined);
}

assert.ok(retained.length > 0);
