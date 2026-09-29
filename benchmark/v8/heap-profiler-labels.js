'use strict';

// Allocation overhead of the sampling heap profiler with and without labels.

const common = require('../common.js');
const v8 = require('v8');

const bench = common.createBenchmark(main, {
  mode: ['none', 'sampling', 'sampling-with-labels', 'sampling-with-groups'],
  n: [1e6],
});

function main({ mode, n }) {
  const interval = 512 * 1024; // V8 default.

  let handle;
  if (mode === 'sampling') {
    handle = v8.startHeapProfile({ sampleInterval: interval });
  } else if (mode === 'sampling-with-labels') {
    handle = v8.startHeapProfile({ labels: true, sampleInterval: interval });
  } else if (mode === 'sampling-with-groups') {
    handle = v8.startHeapProfile({
      labels: true, groupBy: ['route'], sampleInterval: interval,
    });
  }

  if (mode === 'sampling-with-labels' || mode === 'sampling-with-groups') {
    v8.withHeapProfileLabels({ route: '/bench' }, () => {
      runWorkload(n);
    });
  } else {
    runWorkload(n);
  }

  if (handle) {
    handle.stop();
  }
}

function runWorkload(n) {
  const arr = [];
  bench.start();
  for (let i = 0; i < n; i++) {
    arr.push({ id: i, name: `item-${i}`, value: Math.random() });
    // Bound the live set while keeping GC pressure.
    if (arr.length > 1000) arr.shift();
  }
  bench.end(n);
}
