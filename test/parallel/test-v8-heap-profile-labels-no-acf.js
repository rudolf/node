// Flags: --no-async-context-frame
'use strict';
const common = require('../common');
const assert = require('assert');
const v8 = require('v8');

common.expectWarning(
  'Warning',
  'Heap profile labels require async context frames, which are disabled ' +
  'by --no-async-context-frame. All labels will be empty.',
  'NODE_HEAP_PROFILE_LABELS_NO_ASYNC_CONTEXT_FRAME');

const handle = v8.startHeapProfile({ sampleInterval: 64, labels: true });
const retained = [];
v8.withHeapProfileLabels({ route: '/a' }, () => {
  for (let i = 0; i < 2000; i++) retained.push({ i });
});
v8.setHeapProfileLabels({ route: '/b' });
const { samples } = handle.getAllocationProfile();
assert.ok(samples.length > 0);
for (const sample of samples) assert.deepStrictEqual(sample.labels, {});
handle.stop();
