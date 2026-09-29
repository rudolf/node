'use strict';
// Labelled heap profiling is unsupported while building a startup snapshot.
// Labels are ignored during the build and work after deserialization.
require('../common');
const assert = require('assert');
const fs = require('fs');
const tmpdir = require('../common/tmpdir');
const { buildSnapshot, runWithSnapshot } = require('../common/snapshot');

tmpdir.refresh();
const entry = tmpdir.resolve('entry.js');
fs.writeFileSync(entry, `
  'use strict';
  const assert = require('assert');
  const v8 = require('v8');
  assert.throws(() => v8.startHeapProfile({ labels: true }),
                { code: 'ERR_NOT_SUPPORTED_IN_SNAPSHOT' });
  const handle = v8.startHeapProfile();
  globalThis.keep = v8.withHeapProfileLabels({ phase: 'build' }, () => {
    v8.setHeapProfileLabels({ phase: 'entered' });
    return [{}];
  });
  handle.stop();
  v8.startupSnapshot.setDeserializeMainFunction(() => {
    const handle = v8.startHeapProfile({ sampleInterval: 64, labels: true });
    const retained = [];
    v8.withHeapProfileLabels({ phase: 'run' }, () => {
      for (let i = 0; i < 2000; i++) retained.push({ i });
    });
    const { samples } = handle.getAllocationProfile();
    assert.ok(samples.some((s) => s.labels.phase === 'run'));
    handle.stop();
    console.log('deserialized');
  });
`);

buildSnapshot(entry);
const { stdout } = runWithSnapshot();
assert.match(stdout, /deserialized/);
